#include "Core/Input.h"
#include "Core/Application.h"

#include <GLFW/glfw3.h>

namespace Engine {

    static GLFWwindow* NativeWindow() {
        return Application::Get().GetWindow().GetNativeWindow();
    }

    bool Input::IsKeyDown(int key) {
        int state = glfwGetKey(NativeWindow(), key);
        return state == GLFW_PRESS || state == GLFW_REPEAT;
    }

    bool Input::IsMouseButtonDown(int button) {
        return glfwGetMouseButton(NativeWindow(), button) == GLFW_PRESS;
    }

    std::pair<float, float> Input::GetMousePosition() {
        double x, y;
        glfwGetCursorPos(NativeWindow(), &x, &y);
        return { (float)x, (float)y };
    }

    void Input::SetCursorLocked(bool locked) {
        glfwSetInputMode(NativeWindow(), GLFW_CURSOR,
            locked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    }

} // namespace Engine