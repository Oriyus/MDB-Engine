#include "Core/Application.h"
#include "Core/Profiler.h"
#include "Core/Log.h"
#include "Core/Input.h"
#include "Core/SplashScreen.h"
#include "Renderer/RenderStats.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace Engine {

    Application::Application(const std::string& name) : Application([&] { WindowProps p; p.Title = name; return p; }()) {}

    Application::Application(const WindowProps& props) : m_Name(props.Title) {
        s_Instance = this;

        WindowProps wp = props;
        if (props.Splash) {
            // The splash first, so something shows at once; the main window waits, hidden, until the first frame
            Splash::Show(props.SplashTitle.empty() ? props.Title : props.SplashTitle, props.SplashSubtitle);
            Splash::Report(0.02f, "Starting the graphics device");
            wp.StartHidden = true;
        }
        m_Window = std::make_unique<Window>(wp);
        Splash::Report(0.06f, "Starting the interface");
        m_ImGui = std::make_unique<ImGuiLayer>(m_Window->GetNativeWindow());
        Splash::Report(0.08f, "Loading");
    }

    Application::~Application() {
        Splash::Close();      // if the app ended before its first frame
        m_ImGui.reset();      // must go before the window: it needs the GL context
        m_Window.reset();
        s_Instance = nullptr;
    }

    void Application::SetCommandLineArgs(int argc, char** argv) { s_Args.assign(argv, argv + argc); }
    const std::vector<std::string>& Application::GetCommandLineArgs() { return s_Args; }

    void Application::SetTitle(const std::string& title) {
        m_Name = title;
        m_Window->SetTitle(title);
    }

    void Application::Run() {
        OnStart();
        Splash::Report(0.80f, "Preparing the first frame");   // the sky bake and the first draw: about a fifth of a start
        m_LastTime = glfwGetTime();
#ifdef __EMSCRIPTEN__
        // A browser calls us once per display frame (requestAnimationFrame); Run never returns, so the
        // application object must stay alive (EntryPoint.h keeps it)
        emscripten_set_main_loop_arg([](void* self) {
            Application* app = static_cast<Application*>(self);
            if (!app->Frame()) {
                app->Finish();
                emscripten_cancel_main_loop();
                ENGINE_INFO("The game has ended");
            }
        }, this, 0, true);
#else
        while (Frame()) {}
        Finish();
#endif
    }

    void Application::Finish() {
        if (m_Finished) return;
        m_Finished = true;
        OnShutdown();
    }

    bool Application::Frame() {
        if (!m_Running) return false;
        if (m_Window->ShouldClose()) {
            if (OnCloseRequested()) return false;
            m_Window->SetShouldClose(false);   // the app said no (it may ask the user first)
        }
        double now = glfwGetTime();
        float dt = (float)(now - m_LastTime);
        m_LastTime = now;

        // FPS counter in the title bar
        m_FpsTimer += dt;
        m_FrameCount++;
        if (m_FpsTimer >= 1.0 && m_ShowFpsInTitle) {
            m_Window->SetTitle(m_Name + " | " + std::to_string(m_FrameCount) + " FPS");
            m_FpsTimer = 0.0;
            m_FrameCount = 0;
        }

        RenderStats::Reset();
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        Input::PollGamepads();         // one gamepad snapshot per frame for the app and its scripts
        Input::BeginTextFrame();       // this frame's typing (text fields)
        { PROFILE_SCOPE("Update"); OnUpdate(dt); }

        {
            PROFILE_SCOPE("UI");
            m_ImGui->Begin();
            { PROFILE_SCOPE("Build"); OnImGui(); }
            { PROFILE_SCOPE("Draw"); m_ImGui->End(); }
        }

        { PROFILE_SCOPE("Swap"); m_Window->OnUpdate(); }
        if (!m_Window->IsVisible()) {   // the first frame is drawn: now the window can appear, and the splash go
            Splash::Report(1.0f, "Ready");
            m_Window->Show();
            Splash::Close();
        }
        Profiler::EndFrame();
        return m_Running;
    }

} // namespace Engine