#pragma once
#include "Core/ImGuiLayer.h" 
#include "Core/Window.h"
#include <memory>
#include <string>

namespace Engine {

    class Application {
    public:
        explicit Application(const std::string& name = "MDB Engine");
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

    private:
        std::unique_ptr<Window> m_Window;
        std::unique_ptr<ImGuiLayer> m_ImGui;
        std::string m_Name;
        bool m_Running = true;

        static inline Application* s_Instance = nullptr;
    };

    // Defined by the game (Sandbox)
    std::unique_ptr<Application> CreateApplication();

} // namespace Engine