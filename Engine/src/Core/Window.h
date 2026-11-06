#pragma once
#include <cstdint>
#include <string>

struct GLFWwindow;

namespace Engine {

    struct WindowProps {
        std::string Title = "MDB Engine";
        uint32_t Width = 1280;
        uint32_t Height = 720;
        bool VSync = true;
    };

    class Window {
    public:
        explicit Window(const WindowProps& props = WindowProps());
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        void OnUpdate();                       // poll events + swap buffers
        bool ShouldClose() const;

        void SetTitle(const std::string& title);
        void SetVSync(bool enabled);
        bool IsVSync() const { return m_Data.VSync; }

        uint32_t GetWidth()  const { return m_Data.Width; }
        uint32_t GetHeight() const { return m_Data.Height; }
        GLFWwindow* GetNativeWindow() const { return m_Window; }

    private:
        GLFWwindow* m_Window = nullptr;
        WindowProps m_Data;
    };

} // namespace Engine