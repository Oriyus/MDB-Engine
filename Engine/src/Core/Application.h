#pragma once
#include "Core/ImGuiLayer.h" 
#include "Core/Window.h"
#include <memory>
#include <string>
#include <vector>

namespace Engine {

    class Application {
    public:
        explicit Application(const std::string& name = "MDB Engine");
        explicit Application(const WindowProps& props);   // title, size, vsync, fullscreen
        virtual ~Application();

        void Run();
        void Close() { m_Running = false; }

        // Override these in your game
        virtual void OnStart() {}
        virtual void OnUpdate(float deltaTime) {}
        virtual void OnImGui() {}
        virtual void OnShutdown() {}

        Window& GetWindow() { return *m_Window; }
        static Application& Get() { return *s_Instance; }

        // The program's command line (argv), set by EntryPoint.h before CreateApplication runs
        static void SetCommandLineArgs(int argc, char** argv);
        static const std::vector<std::string>& GetCommandLineArgs();

    protected:
        bool m_ShowFpsInTitle = true;   // "Title | 60 FPS" once a second

    private:
        std::unique_ptr<Window> m_Window;
        std::unique_ptr<ImGuiLayer> m_ImGui;
        std::string m_Name;
        bool m_Running = true;

        static inline Application* s_Instance = nullptr;
        static inline std::vector<std::string> s_Args;
    };

    // Defined by the game (Sandbox)
    std::unique_ptr<Application> CreateApplication();

} // namespace Engine