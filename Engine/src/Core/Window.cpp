#include "Core/Window.h"
#include "Core/Log.h"
#include "Core/Input.h"

#include <glad/glad.h>      // must come before GLFW
#include <GLFW/glfw3.h>
#ifdef __EMSCRIPTEN__
#include <GLFW/emscripten_glfw3.h>
#include <emscripten/html5.h>
#endif

namespace Engine {

    static void GLFWErrorCallback(int error, const char* description) {
#ifdef __EMSCRIPTEN__
        if (error == 0) return;      // the browser port's warnings about window features a page doesn't have
#endif
        ENGINE_ERROR("GLFW error ({}): {}", error, description);
    }

    Window::Window(const WindowProps& props) : m_Data(props) {
        ENGINE_INFO("Creating window '{}' ({}x{})", props.Title, props.Width, props.Height);

        glfwSetErrorCallback(GLFWErrorCallback);
        if (!glfwInit()) {
            ENGINE_ERROR("Failed to initialize GLFW");
            std::abort();
        }

#ifdef __EMSCRIPTEN__
        // WebGL2 (OpenGL ES 3.0) on the page's canvas, at the display's pixel density
        // (the WebGL version comes from the link flags: -sMIN/MAX_WEBGL_VERSION=2)
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
        glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_TRUE);
#else
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif
        glfwWindowHint(GLFW_VISIBLE, props.StartHidden ? GLFW_FALSE : GLFW_TRUE);

        m_Window = glfwCreateWindow((int)props.Width, (int)props.Height,
            props.Title.c_str(), nullptr, nullptr);
        if (!m_Window) {
            ENGINE_ERROR("Failed to create GLFW window (does your GPU support OpenGL 4.5?)");
            std::abort();
        }

        glfwMakeContextCurrent(m_Window);
#ifdef __EMSCRIPTEN__
        // The canvas fills the browser window and follows its size (Game/web/shell.html styles it full window)
        emscripten_glfw_make_canvas_resizable(m_Window, "window", nullptr);
#endif

        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            ENGINE_ERROR("Failed to initialize glad");
            std::abort();
        }

        ENGINE_INFO("OpenGL vendor:   {}", (const char*)glGetString(GL_VENDOR));
        ENGINE_INFO("OpenGL renderer: {}", (const char*)glGetString(GL_RENDERER));
        ENGINE_INFO("OpenGL version:  {}", (const char*)glGetString(GL_VERSION));

        // Lets callbacks find our Window object
        glfwSetWindowUserPointer(m_Window, this);

        glfwSetFramebufferSizeCallback(m_Window, [](GLFWwindow* w, int width, int height) {
            auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
            self->m_Data.Width = (uint32_t)width;
            self->m_Data.Height = (uint32_t)height;
            glViewport(0, 0, width, height);
            });

        // Typing for text fields (Input::TextEvents). Installed before ImGui's backend, which calls these too.
        glfwSetCharCallback(m_Window, [](GLFWwindow*, unsigned int codepoint) {
            if (codepoint >= 32 && codepoint != 127) Input::QueueTextEvent(TextEvent{ codepoint, 0, 0 });
            });
        glfwSetKeyCallback(m_Window, [](GLFWwindow*, int key, int, int action, int mods) {
            if (action == GLFW_PRESS || action == GLFW_REPEAT) Input::QueueTextEvent(TextEvent{ 0, key, mods });
            });

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        SetVSync(props.VSync);

        // The framebuffer can differ from the requested size (display scaling): use the real one
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(m_Window, &fbw, &fbh);
        m_Data.Width = (uint32_t)fbw;
        m_Data.Height = (uint32_t)fbh;
        glViewport(0, 0, fbw, fbh);

        // Always created windowed, then switched: that way there is a windowed size to return to
        m_Data.Fullscreen = false;
#ifdef __EMSCRIPTEN__
        return;   // a page fills the browser window already; full screen needs a click or key from the player (F11)
#endif
        if (props.Maximized) glfwMaximizeWindow(m_Window);   // GLFW sizes a hidden window to the work area by hand
        if (props.Fullscreen) {
            if (props.StartHidden) m_PendingFullscreen = true; // switching the monitor would show it
            else SetFullscreen(true);
        }
    }

    void Window::Show() {
        if (IsVisible()) return;
        glfwShowWindow(m_Window);                           // and focuses it (GLFW_FOCUS_ON_SHOW)
        if (m_PendingFullscreen) { m_PendingFullscreen = false; SetFullscreen(true); }
    }

    bool Window::IsVisible() const {
#ifdef __EMSCRIPTEN__
        return true;                                       // the page's canvas
#else
        return glfwGetWindowAttrib(m_Window, GLFW_VISIBLE) != 0;
#endif
    }

    void Window::SetFullscreen(bool fullscreen) {
#ifdef __EMSCRIPTEN__
        // The browser's full screen (it allows it only from a key press or click, which is how F11 / the menu ask)
        m_Data.Fullscreen = emscripten_glfw_is_window_fullscreen(m_Window);
        if (fullscreen == m_Data.Fullscreen) return;
        if (fullscreen) emscripten_glfw_request_fullscreen(m_Window, false, true);
        else emscripten_exit_fullscreen();
        m_Data.Fullscreen = fullscreen;
        return;
#endif
        if (fullscreen == m_Data.Fullscreen) return;
        if (fullscreen) {
            // Remember the normal (restored) rect; a maximized window is maximized again on the way back
            m_WindowedMaximized = glfwGetWindowAttrib(m_Window, GLFW_MAXIMIZED) != 0;
            if (m_WindowedMaximized) glfwRestoreWindow(m_Window);
            glfwGetWindowPos(m_Window, &m_WindowedX, &m_WindowedY);
            glfwGetWindowSize(m_Window, &m_WindowedW, &m_WindowedH);
            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            glfwSetWindowMonitor(m_Window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        }
        else {
            glfwSetWindowMonitor(m_Window, nullptr, m_WindowedX, m_WindowedY, m_WindowedW, m_WindowedH, 0);
            if (m_WindowedMaximized) glfwMaximizeWindow(m_Window);
        }
        m_Data.Fullscreen = fullscreen;
        SetVSync(m_Data.VSync);     // some drivers drop the swap interval on a mode change
    }

    Window::~Window() {
        glfwDestroyWindow(m_Window);
        glfwTerminate();
    }

    void Window::OnUpdate() {
#ifndef __EMSCRIPTEN__
        glfwSwapBuffers(m_Window);         // (a browser shows the canvas when the frame callback returns)
#endif
        glfwPollEvents();
    }

    bool Window::ShouldClose() const {
        return glfwWindowShouldClose(m_Window);
    }

    void Window::SetShouldClose(bool close) { glfwSetWindowShouldClose(m_Window, close ? GLFW_TRUE : GLFW_FALSE); }

    void Window::SetTitle(const std::string& title) {
        glfwSetWindowTitle(m_Window, title.c_str());
    }

    void Window::SetVSync(bool enabled) {
        glfwSwapInterval(enabled ? 1 : 0);
        m_Data.VSync = enabled;
    }

} // namespace Engine