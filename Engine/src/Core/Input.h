#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Engine {

    // Common key codes (values match GLFW's, so you can also use GLFW_KEY_* directly)
    namespace Key {
        constexpr int Space = 32;
        constexpr int A = 65, D = 68, S = 83, W = 87;
        constexpr int Escape = 256;
        constexpr int F11 = 300;
        constexpr int Left = 263, Right = 262, Up = 265, Down = 264;
        constexpr int F5 = 294;
        constexpr int E = 69, Q = 81;
        constexpr int LeftShift = 340;
        constexpr int Tab = 258;
        constexpr int F = 70, G = 71;
        constexpr int F1 = 290;
        constexpr int Z = 90, X = 88, C = 67, Y = 89;
        constexpr int LeftControl = 341, RightControl = 345;
        constexpr int Delete = 261;
        constexpr int Num1 = 49, Num2 = 50, Num3 = 51, Num4 = 52;
        constexpr int R = 82;
        constexpr int N = 78;
        constexpr int P = 80;
        constexpr int T = 84;
        constexpr int H = 72;
    }

    // Modifier bits of a key event (GLFW's values)
    namespace Mod {
        constexpr int Shift = 1, Control = 2, Alt = 4, Super = 8;
    }

    // One step of typing, in the order it happened: a character typed (Char != 0, a Unicode code point, after the
    // keyboard layout), or a key pressed or auto-repeated (Key, Mods). Text fields use these: key state polled once
    // a frame would miss quick taps and repeats.
    struct TextEvent {
        uint32_t Char = 0;
        int Key = 0;
        int Mods = 0;
    };

    namespace Mouse {
        constexpr int Left = 0, Right = 1, Middle = 2;
    }

    // Gamepad buttons and axes, in GLFW's gamepad order (an Xbox layout: A is the bottom face button)
    namespace Pad {
        enum Button : int {
            A, B, X, Y, LB, RB, Back, Start, Guide, LS, RS, DPadUp, DPadRight, DPadDown, DPadLeft, ButtonCount
        };
        enum Axis : int { LeftX, LeftY, RightX, RightY, LT, RT, AxisCount };
    }

    // One gamepad as the engine sees it. Axes are converted from GLFW's: stick Y is up-positive (GLFW's is
    // down-positive) and triggers go 0 (released) .. 1 (GLFW's -1 .. 1). No dead zones yet: those depend on the
    // game's settings and are applied by ApplyGamepadDeadZones (Core/InputMap.h).
    struct GamepadState {
        bool Connected = false;
        int Id = -1;                                    // GLFW joystick slot
        std::string Name;
        std::array<bool, Pad::ButtonCount> Buttons{};
        std::array<float, Pad::AxisCount> Axes{};
    };

    class Input {
    public:
        static void SetCursorLocked(bool locked);
        static bool IsKeyDown(int key);
        static bool IsMouseButtonDown(int button);
        static std::pair<float, float> GetMousePosition();

        // Reads the gamepads (Application calls it once per frame, before OnUpdate). The active pad is the one
        // last used: a pad that presses a button takes over; if the active pad is unplugged, the next connected
        // one becomes active. Only devices GLFW has a gamepad mapping for count (XInput pads, most known pads).
        static void PollGamepads();
        static const GamepadState& Gamepad();           // the active pad (Connected false if there is none)

        // Typing: the window's callbacks queue events as they arrive; BeginTextFrame (Application, once per frame,
        // before OnUpdate) makes them this frame's TextEvents.
        static void QueueTextEvent(const TextEvent& e);
        static void BeginTextFrame();
        static const std::vector<TextEvent>& TextEvents();
    };

} // namespace Engine
