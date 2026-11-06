#pragma once

struct GLFWwindow;

namespace Engine {

    class ImGuiLayer {
    public:
        explicit ImGuiLayer(GLFWwindow* window);
        ~ImGuiLayer();

        ImGuiLayer(const ImGuiLayer&) = delete;
        ImGuiLayer& operator=(const ImGuiLayer&) = delete;

        void Begin();   // start a UI frame
        void End();     // render the UI on top of the scene
    };

} // namespace Engine