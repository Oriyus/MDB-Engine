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
        bool Fullscreen = false;       // borderless on the primary monitor, at its resolution
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

        void SetFullscreen(bool fullscreen);   // switch at runtime (keeps the windowed size to return to)
        bool IsFullscreen() const { return m_Data.Fullscreen; }

        uint32_t GetWidth()  const { return m_Data.Width; }
        uint32_t GetHeight() const { return m_Data.Height; }
        GLFWwindow* GetNativeWindow() const { return m_Window; }

    private:
        GLFWwindow* m_Window = nullptr;
        WindowProps m_Data;
        int m_WindowedX = 100, m_WindowedY = 100, m_WindowedW = 1280, m_WindowedH = 720;
    };

} // namespace Engine