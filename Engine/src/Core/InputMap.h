#pragma once
#include "Core/Input.h"

#include <nlohmann/json_fwd.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine {

    // Named input actions ("jump", "move") that keys, mouse buttons and gamepad controls all feed, so game code
    // never names a key. A project defines them in Assets/Data/input.json (InputMap::kPath):
    //
    //   { "deadzone": 0.2, "triggerDeadzone": 0.1,
    //     "actions": {
    //       "jump":   ["key:Space", "pad:A"],                                     a button (short form)
    //       "sprint": { "type": "button", "bindings": ["key:LeftShift", "pad:LT"] },
    //       "turn":   { "type": "axis", "positive": ["key:E", "pad:RightRight"], "negative": ["key:Q", "pad:RightLeft"] },
    //       "move":   { "type": "vector", "bindings": ["key:WASD", "pad:LeftStick"],
    //                   "up": ["key:Up"], "down": ["key:Down"], "left": ["key:Left"], "right": ["key:Right"] } } }
    //
    // Sources: "key:<name>" (A..Z, 0..9, F1..F12, Space, Enter, Escape, Tab, Backspace, Up, Down, Left, Right,
    // LeftShift, LeftCtrl, LeftAlt, RightShift, RightCtrl, RightAlt, Insert, Delete, Home, End, PageUp, PageDown,
    // Minus, Equal, Comma, Period, Slash, Semicolon, Apostrophe, LeftBracket, RightBracket, Backslash, Grave,
    // KP0..KP9), "mouse:Left|Right|Middle", "pad:<button>" (A B X Y LB RB Back Start Guide LS RS DPadUp DPadDown
    // DPadLeft DPadRight) and "pad:<half axis>" (LeftUp LeftDown LeftLeft LeftRight, the same with Right, LT, RT).
    // A vector's "bindings" also take the groups key:WASD, key:Arrows, pad:LeftStick, pad:RightStick, pad:DPad.
    //
    // Each source gives 0..1 (an axis half: how far it is pushed that way, after the dead zone). A button is the
    // strongest of its sources; an axis is positive minus negative (-1..1); a vector is (right - left,
    // up - down), shortened to length 1. An action is down while its strength (|value|, the vector's length)
    // is at least 0.5, and stays down until it falls below 0.35, so a trigger held near the middle doesn't
    // flicker.

    // Radial dead zone on both sticks (rescaled so just outside it is 0 and full tilt 1) and a plain one on the
    // triggers. Axes as GamepadState holds them (stick Y up, triggers 0..1).
    void ApplyGamepadDeadZones(GamepadState& pad, float stick, float trigger);

    struct InputSource {
        enum class Kind : uint8_t { Key, Mouse, PadButton, PadAxis };
        Kind K = Kind::Key;
        int Code = 0;            // GLFW key, mouse button, Pad::Button or Pad::Axis
        float Sign = 1.0f;       // PadAxis: which half (+1 / -1; triggers +1)
    };

    bool ParseInputSource(const std::string& text, InputSource& out);
    std::string InputSourceName(const InputSource& s);       // back to "key:Space" etc.
    int KeyFromName(const std::string& name);                // -1 if unknown
    const char* KeyName(int key);                            // nullptr if it has no name
    int PadButtonFromName(const std::string& name);          // -1 if unknown
    int PadAxisFromName(const std::string& name);            // LeftX LeftY RightX RightY LT RT, -1 if unknown
    const char* PadButtonName(int b);
    const char* PadAxisName(int a);

    enum class InputActionType : uint8_t { Button, Axis, Vector };
    const char* InputActionTypeName(InputActionType t);      // "button", "axis", "vector"

    struct InputAction {
        std::string Name;
        InputActionType Type = InputActionType::Button;
        // Button: [0]. Axis: [0] positive, [1] negative. Vector: [0] up, [1] down, [2] left, [3] right
        std::array<std::vector<InputSource>, 4> Slots;

        // Results of the last Update
        float X = 0.0f, Y = 0.0f;      // Axis: X. Vector: X, Y. Button: X = strength
        float Strength = 0.0f;         // 0..1
        bool Down = false, WasDown = false;
    };

    // One frame of raw input: key states indexed by GLFW key code, mouse buttons, and the gamepad with its dead
    // zones already applied
    struct InputFrame {
        const bool* Keys = nullptr;  int KeyCount = 0;
        const bool* Mouse = nullptr; int MouseCount = 0;
        const GamepadState* Pad = nullptr;
    };

    class InputMap {
    public:
        static constexpr const char* kPath = "Data/input.json";   // relative to the project's Assets

        float StickDeadZone = 0.2f, TriggerDeadZone = 0.1f;

        void Clear();
        // Bad sources or actions are skipped with a warning each; false (and no actions) if `j` isn't an object
        bool Load(const nlohmann::json& j, std::vector<std::string>& warnings);
        // A missing file is fine (no actions, returns true); a malformed one sets `error` and returns false
        bool LoadFile(const std::string& file, std::string& error, std::vector<std::string>& warnings);
        bool HasFile() const { return m_HasFile; }

        void Update(const InputFrame& frame);         // once per frame: values and down / pressed edges
        const InputAction* Find(const std::string& name) const;
        const std::vector<InputAction>& Actions() const { return m_Actions; }
        std::string Names() const;                    // "jump, move, sprint" (for error messages)

        // The starter file New Project writes (move, look, jump, sprint, interact, confirm, cancel, pause)
        static const char* DefaultJson();

    private:
        std::vector<InputAction> m_Actions;           // sorted by name
        std::unordered_map<std::string, size_t> m_Index;
        bool m_HasFile = false;
    };

} // namespace Engine
