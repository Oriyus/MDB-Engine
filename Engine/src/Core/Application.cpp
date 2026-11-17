#include "Core/Application.h"
#include "Core/Profiler.h"
#include "Core/Log.h"
#include "Core/Input.h"
#include "Core/SplashScreen.h"
#include "Renderer/RenderStats.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>

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

        double lastTime = glfwGetTime();
        double fpsTimer = 0.0;
        int frameCount = 0;

        while (m_Running) {
            if (m_Window->ShouldClose()) {
                if (OnCloseRequested()) break;
                m_Window->SetShouldClose(false);   // the app said no (it may ask the user first)
            }
            double now = glfwGetTime();
            float dt = (float)(now - lastTime);
            lastTime = now;

            // FPS counter in the title bar
            fpsTimer += dt;
            frameCount++;
            if (fpsTimer >= 1.0 && m_ShowFpsInTitle) {
                m_Window->SetTitle(m_Name + " | " + std::to_string(frameCount) + " FPS");
                fpsTimer = 0.0;
                frameCount = 0;
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
        }

        OnShutdown();
    }

} // namespace Engine