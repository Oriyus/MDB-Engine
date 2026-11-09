#include "Core/Application.h"
#include "Core/Profiler.h"
#include "Core/Log.h"
#include "Renderer/RenderStats.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace Engine {

    Application::Application(const std::string& name) : Application([&] { WindowProps p; p.Title = name; return p; }()) {}

    Application::Application(const WindowProps& props) : m_Name(props.Title) {
        s_Instance = this;

        m_Window = std::make_unique<Window>(props);
        m_ImGui = std::make_unique<ImGuiLayer>(m_Window->GetNativeWindow());
    }

    Application::~Application() {
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

            { PROFILE_SCOPE("Update"); OnUpdate(dt); }

            {
                PROFILE_SCOPE("UI");
                m_ImGui->Begin();
                { PROFILE_SCOPE("Build"); OnImGui(); }
                { PROFILE_SCOPE("Draw"); m_ImGui->End(); }
            }

            { PROFILE_SCOPE("Swap"); m_Window->OnUpdate(); }
            Profiler::EndFrame();
        }

        OnShutdown();
    }

} // namespace Engine