#include "Core/Input.h"
#include "Core/Application.h"
#include "Core/Log.h"

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

    // ---- gamepads

    static GamepadState s_Pad;

    static bool ReadPad(int jid, GamepadState& out) {
        GLFWgamepadstate st;
        if (!glfwJoystickIsGamepad(jid) || !glfwGetGamepadState(jid, &st)) return false;
        out.Connected = true;
        out.Id = jid;
        const char* name = glfwGetGamepadName(jid);
        out.Name = name ? name : "Gamepad";
        for (int b = 0; b < Pad::ButtonCount; b++) out.Buttons[b] = st.buttons[b] == GLFW_PRESS;
        out.Axes[Pad::LeftX]  =  st.axes[GLFW_GAMEPAD_AXIS_LEFT_X];
        out.Axes[Pad::LeftY]  = -st.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];       // up-positive
        out.Axes[Pad::RightX] =  st.axes[GLFW_GAMEPAD_AXIS_RIGHT_X];
        out.Axes[Pad::RightY] = -st.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y];
        out.Axes[Pad::LT] = (st.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] + 1.0f) * 0.5f;    // -1..1 -> 0..1
        out.Axes[Pad::RT] = (st.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.0f) * 0.5f;
        return true;
    }

    static bool AnyButton(const GamepadState& p) {
        for (bool b : p.Buttons) if (b) return true;
        return false;
    }

    void Input::PollGamepads() {
        const int before = s_Pad.Connected ? s_Pad.Id : -1;
        const std::string beforeName = s_Pad.Name;
        // The active pad stays active while it is plugged in, unless it is idle and another pad presses a button
        GamepadState current, first, taker;
        for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; jid++) {
            GamepadState p;
            if (!ReadPad(jid, p)) continue;
            if (!first.Connected) first = p;
            if (jid == before) current = p;
            else if (!taker.Connected && AnyButton(p)) taker = p;
        }
        if (taker.Connected && !(current.Connected && AnyButton(current))) s_Pad = taker;
        else if (current.Connected) s_Pad = current;
        else s_Pad = first;                               // the active pad is gone (or there was none)

        const int after = s_Pad.Connected ? s_Pad.Id : -1;
        if (after != before) {
            if (after >= 0) ENGINE_INFO("Gamepad: {} (slot {})", s_Pad.Name, after + 1);
            else            ENGINE_INFO("Gamepad disconnected: {}", beforeName);
        }
    }

    const GamepadState& Input::Gamepad() { return s_Pad; }

} // namespace Engine
