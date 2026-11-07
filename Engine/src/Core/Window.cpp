#include "Core/Window.h"
#include "Core/Log.h"

#include <glad/glad.h>      // must come before GLFW
#include <GLFW/glfw3.h>

namespace Engine {

    static void GLFWErrorCallback(int error, const char* description) {
        ENGINE_ERROR("GLFW error ({}): {}", error, description);
    }

    Window::Window(const WindowProps& props) : m_Data(props) {
        ENGINE_INFO("Creating window '{}' ({}x{})", props.Title, props.Width, props.Height);

        glfwSetErrorCallback(GLFWErrorCallback);
        if (!glfwInit()) {
            ENGINE_ERROR("Failed to initialize GLFW");
            std::abort();
        }

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        m_Window = glfwCreateWindow((int)props.Width, (int)props.Height,
            props.Title.c_str(), nullptr, nullptr);
        if (!m_Window) {
            ENGINE_ERROR("Failed to create GLFW window (does your GPU support OpenGL 4.5?)");
            std::abort();
        }

        glfwMakeContextCurrent(m_Window);

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
        if (props.Fullscreen) SetFullscreen(true);
    }

    void Window::SetFullscreen(bool fullscreen) {
        if (fullscreen == m_Data.Fullscreen) return;
        if (fullscreen) {
            glfwGetWindowPos(m_Window, &m_WindowedX, &m_WindowedY);
            glfwGetWindowSize(m_Window, &m_WindowedW, &m_WindowedH);
            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            glfwSetWindowMonitor(m_Window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        }
        else {
            glfwSetWindowMonitor(m_Window, nullptr, m_WindowedX, m_WindowedY, m_WindowedW, m_WindowedH, 0);
        }
        m_Data.Fullscreen = fullscreen;
        SetVSync(m_Data.VSync);     // some drivers drop the swap interval on a mode change
    }

    Window::~Window() {
        glfwDestroyWindow(m_Window);
        glfwTerminate();
    }

    void Window::OnUpdate() {
        glfwSwapBuffers(m_Window);
        glfwPollEvents();
    }

    bool Window::ShouldClose() const {
        return glfwWindowShouldClose(m_Window);
    }

    void Window::SetTitle(const std::string& title) {
        glfwSetWindowTitle(m_Window, title.c_str());
    }

    void Window::SetVSync(bool enabled) {
        glfwSwapInterval(enabled ? 1 : 0);
        m_Data.VSync = enabled;
    }

} // namespace Engine