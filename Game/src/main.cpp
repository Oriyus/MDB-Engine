// The standalone game: plays one scene full screen (or in a window) with no editor.
//
// The game is a project (a folder with project.json and Assets/, see Core/Project.h). Settings come from
// project.json, and the command line can override them:
//   Game.exe [--project <folder>] [--scene <file in Assets/Scenes>] [--fullscreen | --windowed] [--width N]
//            [--height N] [--no-vsync] [--quit-after <seconds>] [--save-dir <folder>] [--settings <file>]
// Save slots go to %APPDATA%/<title>/Saves unless --save-dir says otherwise, and the player's settings (volumes)
// to %APPDATA%/<title>/settings.json (with --save-dir: settings.json in that folder; --settings names the file).
// Where things are: a packaged game (File > Build Game in the editor, or the package_game target) has
// project.json, Assets and EngineAssets next to the exe. Otherwise (running from the build) the project is
// --project or the source tree's Examples, and the engine files are the source tree's EngineAssets.
//
// Keys: Esc pauses (a menu with Resume / Settings / Restart / Quit buttons, worked by mouse, arrows + Enter or the
// pad's D-pad / stick + A; Esc, R and Q work too; Settings has an Audio tab, a volume slider and mute per bus,
// and a Controls tab that rebinds the input actions for keyboard / mouse and gamepad; both saved in settings.json),
// F11 or Alt+Enter toggles full screen.
// Everything else belongs to the game's scripts.

#include "Core/Application.h"
#include "Core/EntryPoint.h"
#include "Core/Input.h"
#include "Core/InputMap.h"
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
#include "Runtime/UILayout.h"
#include "Audio/AudioEngine.h"

#include <glad/glad.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
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
        std::string SettingsFile;             // the player's settings: %APPDATA%/<title>/settings.json (see the top)
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
            else if (a == "--settings" && hasValue) c.SettingsFile = args[++i];
            else LOG_WARN("Unknown argument '{}'", a);
        }
        if (c.SaveDir.empty()) {
            // %APPDATA%/<title>/Saves (the title without characters Windows refuses in names)
            std::string title;
            for (char ch : c.Window.Title) if (std::string("<>:\"/\\|?*").find(ch) == std::string::npos) title += ch;
            if (title.empty()) title = "MDB Game";
            char* appData = nullptr;
            size_t len = 0;
            const fs::path root = (_dupenv_s(&appData, &len, "APPDATA") == 0 && appData) ? fs::path(appData) / title : exeDir;
            std::free(appData);
            c.SaveDir = (root / "Saves").generic_string();
            if (c.SettingsFile.empty()) c.SettingsFile = (root / "settings.json").generic_string();
        }
        if (c.SettingsFile.empty()) c.SettingsFile = (fs::path(c.SaveDir) / "settings.json").generic_string();   // --save-dir given
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
        m_Menu.SetFonts(m_Fonts.Bold, m_Fonts.Regular);
        if (LoadAudioMix(m_Cfg.SettingsFile, m_SavedMix)) m_Session->Audio().SetMix(m_SavedMix);   // the player's volumes
        else m_SavedMix = m_Session->Audio().GetMix();
        nlohmann::json bindings;                 // ... and controls (applied when the scripts start)
        if (LoadInputBindings(m_Cfg.SettingsFile, bindings)) m_Session->Scripts().SetInputOverrides(bindings);

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
        m_SavedInput = m_Session->Scripts().InputOverrides();   // what applied of the file (stale entries dropped)
        m_Running = true;
        // Test hook: start paused on a page of the menu (screen captures of it): pause, audio or controls
        if (const char* page = std::getenv("MDB_PAUSE_MENU")) {
            const std::string p = page;
            m_Paused = true;
            if (p == "audio" || p == "controls") { m_MenuPage = MenuPage::Settings; m_SettingsTab = p == "controls" ? 1 : 0; }
        }
        LOG_INFO("Playing {} (saves in {})", m_Cfg.Scene, m_Cfg.SaveDir);
    }

    void OnUpdate(float dt) override {
        using namespace Engine;
        dt = std::min(dt, 0.1f);     // a long hitch (window drag, environment bake) shouldn't fling the game
        m_Time += dt;
        if (m_Cfg.QuitAfter > 0.0f && m_Time >= m_Cfg.QuitAfter) Close();
        // Settings > Controls waiting for a key / button: every key goes to it, not to the shell
        const bool binding = UpdateCapture();

        // ---- the few keys the game shell keeps for itself
        const bool alt = Input::IsKeyDown(342) || Input::IsKeyDown(346);   // left / right Alt
        // The gamepad's Start works like Esc, and B backs out of the menu (only while paused: in play B is the game's)
        const GamepadState& pad = Input::Gamepad();
        // Which device was used last decides whether the pause menu shows its focus ring at once
        if (pad.Connected) {
            for (bool b : pad.Buttons) m_PadLast = m_PadLast || b;
            for (int a = 0; a < Pad::LT; a++) m_PadLast = m_PadLast || std::abs(pad.Axes[a]) > 0.5f;
        }
        if (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f) m_PadLast = false;
        const bool start = PadEdge(pad.Connected && pad.Buttons[Pad::Start], m_StartDown);
        const bool back = PadEdge(pad.Connected && pad.Buttons[Pad::B], m_BDown) && m_Paused;
        // A script text field with the keyboard (ui.input) owns Escape: it cancels the typing, not the game
        const bool typing = m_Running && !m_Paused && m_Session->Scripts().WantsKeyboard();
        // ... and a closable script panel (inventory, a shop) takes it as Back (pad Start still pauses)
        const bool uiBack = m_Running && !m_Paused && m_Session->Scripts().WantsBack();
        const bool escKey = Edge(Key::Escape, m_EscDown) && !typing && !uiBack;
        if (escKey && !m_Running) { Close(); return; }   // nothing to pause: the scene did not load
        const bool esc = (escKey || start || back) && !binding;
        if (esc && m_Paused && m_MenuPage == MenuPage::Settings) LeaveSettings();   // Esc: back to the menu
        else if (esc) { m_Paused = !m_Paused; m_MenuPage = MenuPage::Main; }
        if (Edge(Key::F11, m_F11Down) || (alt && Edge(257, m_EnterDown)))  // F11, Alt+Enter
            GetWindow().SetFullscreen(!GetWindow().IsFullscreen());
        bool r = Edge(Key::R, m_RDown), q = Edge(Key::Q, m_QDown);
        if (m_MenuPage != MenuPage::Main || binding) r = q = false;
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
            m_Renderer->ResetTemporalHistory();   // a new level: a camera cut
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

        if (m_Paused) {
            dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 150));
            DrawMenu(dl, size);
        }
        else { m_MenuPage = MenuPage::Main; m_MenuOpen = false; }
    }

    // The player's volumes outlive the run: written when they leave Settings and when the game closes
    void OnShutdown() override { SaveMix(); SaveInput(); }

    // The pause menu is laid-out UI (Runtime/UILayout) of its own, so a mouse, the arrow keys + Enter and a gamepad
    // (D-pad / stick + A) all work it. Esc and the pad's B stay the shell's (resume / back), handled in OnUpdate.
    void DrawMenu(ImDrawList* dl, const ImVec2& size) {
        using namespace Engine;
        if (!m_MenuOpen) { m_Menu.Reset(); m_MenuOpen = true; }   // fresh focus: the first button takes it
        const ImGuiIO& io = ImGui::GetIO();
        UIPointerState p;
        p.X = io.MousePos.x / std::max(size.x, 1.0f);
        p.Y = io.MousePos.y / std::max(size.y, 1.0f);
        p.Inside = p.X >= 0.0f && p.X <= 1.0f && p.Y >= 0.0f && p.Y <= 1.0f;
        p.Down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        UINavInput nav = UINavFromDevices(false);
        nav.Gamepad = m_PadLast;
        // While a binding is being captured (and until everything is let go) the menu gets no input: the key or
        // button pressed is the binding, not a move or a click
        const bool binding = m_Capture.Active() || m_InputWait;
        if (binding) { nav = UINavInput{}; nav.Gamepad = m_PadLast; p.Down = false; }
        m_MenuItems.clear();
        m_Menu.SetTextEvents(binding ? std::vector<TextEvent>{} : Input::TextEvents());   // Tab / Shift+Tab
        m_Menu.SetNav(nav);
        m_Menu.BeginFrame(m_MenuItems, size.x / std::max(size.y, 1.0f), p, io.DeltaTime);
        if (m_MenuPage == MenuPage::Settings) DrawSettingsMenu();
        else DrawPauseMenu();
        m_Menu.EndFrame();
        DrawScriptUI(dl, m_Fonts, m_MenuItems, ImVec2(0, 0), size, nullptr);
    }

    static Engine::UITextOptions MenuLabel(float size, bool muted = false) {
        Engine::UITextOptions t;
        t.Size = size;
        t.Align = 1;
        t.Bold = !muted;
        t.Muted = muted;
        t.Shadow = !muted;
        return t;
    }

    void DrawPauseMenu() {
        using namespace Engine;
        UIPanelOptions po;
        po.X = 0.5f; po.Y = 0.47f; po.W = 380.0f;
        po.Padding = 22.0f;
        m_Menu.BeginPanel("pause", po);
        m_Menu.Label("Paused", MenuLabel(48.0f));
        m_Menu.Space(6.0f);
        const char* labels[] = { "Resume", "Settings", "Restart", "Quit" };
        int clicked = -1;
        for (int i = 0; i < 4; i++) {
            UIButtonOptions b;
            b.Fill = true;
            b.H = 50.0f;
            b.Size = 24.0f;
            if (m_Menu.Button(labels[i], b)) clicked = i;
        }
        m_Menu.Space(4.0f);
        m_Menu.Label("Esc resume  \xC2\xB7  R restart  \xC2\xB7  Q quit  \xC2\xB7  F11 full screen", MenuLabel(15.0f, true));   // UTF-8 middle dots
        m_Menu.EndPanel();

        // Acted on next frame by OnUpdate (or right away for quitting)
        if (clicked == 0) m_Paused = false;
        if (clicked == 1) m_MenuPage = MenuPage::Settings;
        if (clicked == 2) { m_RestartPending = true; m_Paused = false; }
        if (clicked == 3) Close();
    }

    // Settings: Audio (a slider and a mute button for the master volume and every bus) and Controls (bindings)
    void DrawSettingsMenu() {
        using namespace Engine;
        UIPanelOptions po;
        po.X = 0.5f; po.Y = 0.47f; po.W = 760.0f;
        po.Padding = 22.0f;
        m_Menu.BeginPanel("settings", po);
        m_Menu.Label("Settings", MenuLabel(44.0f));
        m_SettingsTab = m_Menu.Tabs("settings_tab", { "Audio", "Controls" }, m_SettingsTab);
        m_Menu.Space(6.0f);
        if (m_SettingsTab == 1) DrawControlsTab();
        else DrawAudioTab();

        m_Menu.Space(6.0f);
        UIButtonOptions back;
        back.Fill = true;
        back.H = 50.0f;
        back.Size = 24.0f;
        back.Disabled = m_Capture.Active();
        const bool leave = m_Menu.Button("Back", back);
        m_Menu.Label("Esc back", MenuLabel(15.0f, true));
        m_Menu.EndPanel();
        if (leave) LeaveSettings();
    }

    void DrawAudioTab() {
        using namespace Engine;
        AudioEngine& audio = m_Session->Audio();
        AudioMix mix = audio.GetMix();
        m_Menu.Label("Volume", MenuLabel(20.0f, true));
        m_Menu.Space(4.0f);

        bool changed = false;
        for (int r = 0; r < 1 + kAudioBusCount; r++) {
            const bool master = r == 0;
            const int bus = r - 1;
            float& volume = master ? mix.Master : mix.Volume[bus];
            bool& muted = master ? mix.MasterMuted : mix.Muted[bus];
            const std::string name = master ? "Master" : AudioBusLabel((AudioBus)bus);
            m_Menu.BeginRow(14.0f);
            UITextOptions t;
            t.Size = 20.0f;
            t.Width = 110.0f;
            t.Align = 2;
            t.Bold = true;
            t.Muted = muted;
            m_Menu.Label(name, t);
            UISliderOptions s;
            s.W = 300.0f;
            s.H = 26.0f;
            char pct[16];
            std::snprintf(pct, sizeof pct, "%d%%", (int)std::lround(volume * 100.0f));
            s.Text = pct;
            if (muted) { s.HasColor = true; s.Color = glm::vec3(0.3f); }
            const UISliderResult v = m_Menu.Slider("vol:" + name, volume, s);
            if (v.Changed) {
                volume = v.Value;
                if (muted && v.Value > 0.0f) muted = false;    // turning it up unmutes
                changed = true;
            }
            UIButtonOptions b;
            b.Id = "mute:" + name;
            b.W = 90.0f;
            b.H = 32.0f;
            b.Size = 18.0f;
            b.HasColor = muted;
            b.Color = glm::vec3(0.35f, 0.16f, 0.16f);
            if (m_Menu.Button(muted ? "Muted" : "On", b)) { muted = !muted; changed = true; }
            m_Menu.EndRow();
        }
        if (changed) audio.SetMix(mix);
    }

    // Controls: one row per rebindable action slot ("Jump", "Move up"), a keyboard / mouse cell and a gamepad cell.
    // Choosing a cell waits for the next key or button (InputCapture, run by UpdateCapture), which replaces that
    // device's bindings in that slot. The bindings are overrides on top of the project's input.json, kept in the
    // settings file's "input" key.
    void DrawControlsTab() {
        using namespace Engine;
        const InputMap& map = m_Session->Scripts().InputActions();
        constexpr float kLabelW = 210.0f, kCellW = 240.0f, kRowGap = 12.0f;

        auto header = [&](const std::string& text, float w) {
            UITextOptions t;
            t.Size = 17.0f; t.Width = w; t.Align = 1; t.Muted = true;
            m_Menu.Label(text, t);
        };
        m_Menu.BeginRow(kRowGap);
        header("", kLabelW);
        header("Keyboard / mouse", kCellW);
        header("Gamepad", kCellW);
        m_Menu.EndRow();

        int rows = 0;
        m_Menu.BeginScroll("controls", 250.0f);
        for (const InputAction* ap : map.InFileOrder()) {
            const InputAction& a = *ap;
            if (!a.Rebind) continue;
            const int slots = InputSlotCount(a.Type);
            for (int s = 0; s < slots; s++) {
                rows++;
                m_Menu.BeginRow(kRowGap);
                UITextOptions t;
                t.Size = 19.0f; t.Width = kLabelW; t.Align = 2; t.Bold = true;
                m_Menu.Label(slots == 1 ? a.DisplayLabel() : a.DisplayLabel() + " " + InputSlotKey(a.Type, s), t);
                for (int dev = 0; dev < 2; dev++) {
                    const bool gamepad = dev == 1;
                    const bool capturing = m_Capture.Active() && m_Capture.Gamepad() == gamepad &&
                                           m_CaptureAction == a.Name && m_CaptureSlot == s;
                    std::string text;
                    for (const InputSource& src : a.Slots[s]) {
                        if (src.IsGamepad() != gamepad) continue;
                        if (!text.empty()) text += " / ";
                        text += InputSourceLabel(src);
                    }
                    UIButtonOptions b;
                    b.Id = "bind:" + a.Name + ":" + std::to_string(s) + (gamepad ? ":pad" : ":key");
                    b.W = kCellW; b.H = 34.0f; b.Size = 17.0f;
                    b.Selected = capturing;
                    if (capturing) text = "...";
                    else if (text.empty()) { text = "(none)"; b.HasColor = true; b.Color = glm::vec3(0.16f, 0.17f, 0.2f); }
                    if (m_Menu.Button(text, b) && !m_Capture.Active() && !m_InputWait) {
                        m_Capture.Begin(gamepad);
                        m_CaptureAction = a.Name;
                        m_CaptureSlot = s;
                        m_ControlsNote.clear();
                    }
                }
                m_Menu.EndRow();
            }
        }
        if (rows == 0) m_Menu.Label("This game has no controls to change.", MenuLabel(19.0f, true));
        m_Menu.EndScroll();

        // What is going on: the capture's keys, or what the last change did
        std::string note = m_ControlsNote;
        if (m_Capture.Active())
            note = m_Capture.Gamepad() ? "Press a button or push a stick  \xC2\xB7  Start cancels  \xC2\xB7  Back clears"
                                       : "Press a key or mouse button  \xC2\xB7  Esc cancels  \xC2\xB7  Delete clears";
        m_Menu.Space(4.0f);
        UITextOptions nt = MenuLabel(17.0f, !m_Capture.Active());
        nt.Shadow = false;
        m_Menu.Label(note.empty() ? " " : note, nt);
        m_Menu.Space(2.0f);
        UIButtonOptions reset;
        reset.Fill = true;
        reset.H = 40.0f;
        reset.Size = 20.0f;
        reset.Disabled = m_Capture.Active() || m_Session->Scripts().InputOverrides().empty();
        if (m_Menu.Button("Reset to defaults", reset)) {
            m_Session->Scripts().SetInputOverrides(nlohmann::json::object());
            m_ControlsNote = "Controls are back to the game's defaults.";
        }
    }

    // The raw devices for capturing a binding (the gamepad with the game's dead zones)
    Engine::InputFrame RawInputFrame() {
        using namespace Engine;
        for (int k = 32; k < kRawKeys; k++) m_RawKeys[k] = Input::IsKeyDown(k);
        for (int b = 0; b < 3; b++) m_RawMouse[b] = Input::IsMouseButtonDown(b);
        m_RawPad = Input::Gamepad();
        const InputMap& map = m_Session->Scripts().InputActions();
        ApplyGamepadDeadZones(m_RawPad, map.StickDeadZone, map.TriggerDeadZone);
        InputFrame f;
        f.Keys = m_RawKeys.data(); f.KeyCount = kRawKeys;
        f.Mouse = m_RawMouse.data(); f.MouseCount = 3;
        f.Pad = &m_RawPad;
        return f;
    }

    // Runs a binding capture; true while one runs or the keys that ended it are still held (the shell ignores keys)
    bool UpdateCapture() {
        using namespace Engine;
        if (!m_Session || (!m_Capture.Active() && !m_InputWait)) return false;
        const InputFrame f = RawInputFrame();
        if (!m_Capture.Active()) { m_InputWait = InputCapture::AnyHeld(f); return true; }
        InputSource src;
        const InputCapture::Result r = m_Capture.Update(f, src);
        if (r == InputCapture::Result::Waiting) return true;
        m_InputWait = true;
        if (r == InputCapture::Result::Cancelled) { m_ControlsNote.clear(); return true; }

        ScriptEngine& scripts = m_Session->Scripts();
        InputMap map = scripts.InputActions();
        const bool captured = r == InputCapture::Result::Captured;
        map.Rebind(m_CaptureAction, m_CaptureSlot, m_Capture.Gamepad(), captured ? &src : nullptr);
        scripts.SetInputOverrides(map.OverridesFrom(scripts.DefaultInputActions()));
        m_ControlsNote.clear();
        if (captured) {
            const std::vector<std::string> users = map.UsersOf(src, m_CaptureAction, m_CaptureSlot);
            if (!users.empty()) {
                m_ControlsNote = InputSourceLabel(src) + " is also used for ";
                for (size_t i = 0; i < users.size(); i++) m_ControlsNote += (i ? ", " : "") + users[i];
            }
        }
        return true;
    }

    void LeaveSettings() {
        m_MenuPage = MenuPage::Main;
        m_Capture.Cancel();
        m_ControlsNote.clear();
        SaveMix();
        SaveInput();
    }

    void SaveInput() {
        if (!m_Session) return;
        const nlohmann::json& now = m_Session->Scripts().InputOverrides();
        if (now != m_SavedInput && Engine::SaveInputBindings(m_Cfg.SettingsFile, now)) m_SavedInput = now;
    }

    void SaveMix() {
        if (!m_Session) return;
        const Engine::AudioMix mix = m_Session->Audio().GetMix();
        if (mix != m_SavedMix && Engine::SaveAudioMix(m_Cfg.SettingsFile, mix)) m_SavedMix = mix;
    }

    static Engine::ScriptUIItem MenuText(const std::string& text, float x, float y, float size, int align,
                                         const glm::vec3& color = glm::vec3(1.0f)) {
        Engine::ScriptUIItem t;
        t.Text = text;
        t.X = x; t.Y = y; t.Size = size; t.Align = align; t.Color = color;
        return t;
    }

private:
    enum class MenuPage { Main, Settings };
    static bool Edge(int key, bool& wasDown) {
        const bool down = Engine::Input::IsKeyDown(key);
        const bool pressed = down && !wasDown;
        wasDown = down;
        return pressed;
    }
    static bool PadEdge(bool down, bool& wasDown) {
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

    Engine::AudioMix m_SavedMix;             // what the settings file holds
    nlohmann::json m_SavedInput = nlohmann::json::object();   // the same for the binding overrides
    int m_SettingsTab = 0;                   // Audio, Controls
    Engine::InputCapture m_Capture;          // Controls: waiting for the key / button to bind
    std::string m_CaptureAction;
    int m_CaptureSlot = 0;
    bool m_InputWait = false;                // a capture ended: keys stay away from the menu and shell until let go
    std::string m_ControlsNote;              // under the list: a clash, or "reset"
    static constexpr int kRawKeys = 349;     // GLFW_KEY_LAST + 1
    std::array<bool, kRawKeys> m_RawKeys{};
    std::array<bool, 3> m_RawMouse{};
    Engine::GamepadState m_RawPad;
    MenuPage m_MenuPage = MenuPage::Main;
    Engine::UIContext m_Menu;                // the pause menu (its own laid-out UI)
    std::vector<Engine::ScriptUIItem> m_MenuItems;
    bool m_MenuOpen = false;                 // drawn last frame (reopening starts the focus afresh)
    bool m_PadLast = false;                  // the gamepad was used last (the menu shows its focus at once)
    bool m_Running = false, m_Paused = false, m_RestartPending = false;
    bool m_EscDown = false, m_F11Down = false, m_EnterDown = false, m_RDown = false, m_QDown = false;
    bool m_StartDown = false, m_BDown = false;
    float m_Time = 0.0f;
};

std::unique_ptr<Engine::Application> Engine::CreateApplication() {
    return std::make_unique<GameApp>(LoadConfig());
}
