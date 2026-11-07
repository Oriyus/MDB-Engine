#pragma once
#include <utility>

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
        constexpr int Z = 90, X = 88, C = 67;
        constexpr int Delete = 261;
        constexpr int Num1 = 49, Num2 = 50, Num3 = 51;
        constexpr int R = 82;
        constexpr int N = 78;
        constexpr int P = 80;
        constexpr int T = 84;
    }

    namespace Mouse {
        constexpr int Left = 0, Right = 1, Middle = 2;
    }

    class Input {
    public:
        static void SetCursorLocked(bool locked);
        static bool IsKeyDown(int key);
        static bool IsMouseButtonDown(int button);
        static std::pair<float, float> GetMousePosition();
    };

} // namespace Engine