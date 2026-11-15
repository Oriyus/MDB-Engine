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
        bool Maximized = false;        // windowed, filling the work area (Width/Height are the restored size)
        bool StartHidden = false;      // created invisible (Show() later): Application does this behind a splash
        // A splash window with a load bar while the app starts (Core/SplashScreen.h). The main window stays hidden
        // through OnStart and the first frame. SplashTitle defaults to Title.
        bool Splash = false;
        std::string SplashTitle, SplashSubtitle;
    };

    class Window {
    public:
        explicit Window(const WindowProps& props = WindowProps());
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        void OnUpdate();                       // poll events + swap buffers
        bool ShouldClose() const;
        void SetShouldClose(bool close);

        void SetTitle(const std::string& title);
        void SetVSync(bool enabled);
        bool IsVSync() const { return m_Data.VSync; }

        void SetFullscreen(bool fullscreen);   // switch at runtime (keeps the windowed size to return to)
        bool IsFullscreen() const { return m_Data.Fullscreen; }

        uint32_t GetWidth()  const { return m_Data.Width; }
        uint32_t GetHeight() const { return m_Data.Height; }
        void Show();                           // a StartHidden window appears (maximized or full screen as asked)
        bool IsVisible() const;
        GLFWwindow* GetNativeWindow() const { return m_Window; }

    private:
        GLFWwindow* m_Window = nullptr;
        WindowProps m_Data;
        int m_WindowedX = 100, m_WindowedY = 100, m_WindowedW = 1280, m_WindowedH = 720;
        bool m_WindowedMaximized = false;   // maximized before going full screen: maximized again after
        bool m_PendingFullscreen = false;   // asked for while hidden: done in Show()
    };

} // namespace Engine