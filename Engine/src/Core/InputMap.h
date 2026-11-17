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
    //
    // An action in the object form may also have "label" (what a controls menu shows; default: the name with a
    // capital, "move_left" -> "Move left") and "rebind": false (leave it out of the player's controls menu; the
    // ui_* actions are left out unless they say "rebind": true).
    //
    // The player's own bindings are overrides on top of the file: { "<action>": { "<slot>": [sources] } } with the
    // slot keys of the file's format (bindings / positive, negative / up, down, left, right). The Game keeps them
    // in the "input" key of its settings file; ApplyOverrides replaces only the slots they name.

    // Radial dead zone on both sticks (rescaled so just outside it is 0 and full tilt 1) and a plain one on the
    // triggers. Axes as GamepadState holds them (stick Y up, triggers 0..1).
    void ApplyGamepadDeadZones(GamepadState& pad, float stick, float trigger);

    struct InputSource {
        enum class Kind : uint8_t { Key, Mouse, PadButton, PadAxis };
        Kind K = Kind::Key;
        int Code = 0;            // GLFW key, mouse button, Pad::Button or Pad::Axis
        float Sign = 1.0f;       // PadAxis: which half (+1 / -1; triggers +1)

        bool operator==(const InputSource& o) const { return K == o.K && Code == o.Code && Sign == o.Sign; }
        bool operator!=(const InputSource& o) const { return !(*this == o); }
        bool IsGamepad() const { return K == Kind::PadButton || K == Kind::PadAxis; }
    };

    bool ParseInputSource(const std::string& text, InputSource& out);
    std::string InputSourceName(const InputSource& s);       // back to "key:Space" etc.
    std::string InputSourceLabel(const InputSource& s);      // for players: "Left Shift", "Left Mouse", "D-Pad Up", "Left Stick Up"
    int KeyFromName(const std::string& name);                // -1 if unknown
    const char* KeyName(int key);                            // nullptr if it has no name
    int PadButtonFromName(const std::string& name);          // -1 if unknown
    int PadAxisFromName(const std::string& name);            // LeftX LeftY RightX RightY LT RT, -1 if unknown
    const char* PadButtonName(int b);
    const char* PadAxisName(int a);

    enum class InputActionType : uint8_t { Button, Axis, Vector };
    const char* InputActionTypeName(InputActionType t);      // "button", "axis", "vector"
    int InputSlotCount(InputActionType t);                   // 1, 2, 4
    const char* InputSlotKey(InputActionType t, int slot);   // "bindings" / "positive", "negative" / "up", "down", "left", "right"
    int InputSlotFromKey(InputActionType t, const std::string& key);   // -1 if the type has no such slot

    struct InputAction {
        std::string Name;
        InputActionType Type = InputActionType::Button;
        // Button: [0]. Axis: [0] positive, [1] negative. Vector: [0] up, [1] down, [2] left, [3] right
        std::array<std::vector<InputSource>, 4> Slots;
        std::string Label;             // "label" in the file; empty = made from the name (DisplayLabel)
        bool Rebind = true;            // shown in the player's controls menu ("rebind"; the ui_* default to false)
        int Order = 0;                 // its place in the file (LoadFile; Load alone sorts by name), for menus
        std::string DisplayLabel() const;

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
        std::vector<const InputAction*> InFileOrder() const;   // as input.json lists them (added ones after)

        // The starter file New Project writes (move, look, jump, sprint, interact, confirm, cancel, pause)
        static const char* DefaultJson();
        // The UI navigation actions (an "actions" object: ui_up, ui_down, ui_left, ui_right, ui_confirm, ui_back,
        // ui_secondary), added by ScriptEngine for any the project's file doesn't define
        static const char* UIDefaultJson();
        // Adds the actions of an "actions" object whose names aren't defined yet
        void AddMissing(const nlohmann::json& actions, std::vector<std::string>& warnings);

        // ---- the player's bindings (see the top)
        InputAction* FindMutable(const std::string& name);
        // Replaces the slots `overrides` names; unknown actions, slots and sources are skipped with a warning
        void ApplyOverrides(const nlohmann::json& overrides, std::vector<std::string>& warnings);
        // The slots that differ from `base` (an object; empty when nothing does)
        nlohmann::json OverridesFrom(const InputMap& base) const;
        // One device's part of a slot: replaces every keyboard / mouse source (gamepad = false) or every gamepad
        // source of that slot with `source` (nullptr: removes them). False if there is no such action or slot.
        bool Rebind(const std::string& action, int slot, bool gamepad, const InputSource* source);
        // Rebindable actions (other than `action`'s `slot`) that `source` also feeds: "Jump", "Move up"
        std::vector<std::string> UsersOf(const InputSource& source, const std::string& action, int slot) const;
        // Keeps down / was-down of same-named actions, so swapping maps mid-game doesn't fire presses
        void CopyStateFrom(const InputMap& other);

    private:
        std::vector<InputAction> m_Actions;           // sorted by name
        std::unordered_map<std::string, size_t> m_Index;
        bool m_HasFile = false;
    };

    // Waits for the player to press something to bind (a controls menu). Begin, then Update every frame with the
    // raw input (gamepad dead zones applied) until it returns something other than Waiting. It ignores whatever is
    // held when it starts, so the press that opened it doesn't count.
    //   keyboard: a key or a mouse button; Escape cancels, Delete clears, F11 is the shell's (ignored)
    //   gamepad: a button, or a stick / trigger pushed past 0.6; Start cancels, Back clears (Escape / Delete too)
    class InputCapture {
    public:
        enum class Result { Waiting, Captured, Cancelled, Cleared };
        void Begin(bool gamepad) { m_Active = true; m_Gamepad = gamepad; m_Armed = false; }
        void Cancel() { m_Active = false; }
        bool Active() const { return m_Active; }
        bool Gamepad() const { return m_Gamepad; }
        Result Update(const InputFrame& frame, InputSource& out);
        static bool AnyHeld(const InputFrame& frame);   // anything pressed at all (wait for nothing before taking input again)

    private:
        bool m_Active = false, m_Gamepad = false, m_Armed = false;
    };

    // The "input" key of a settings file (Core/SettingsFile.h): the player's binding overrides
    bool LoadInputBindings(const std::string& file, nlohmann::json& overrides);
    bool SaveInputBindings(const std::string& file, const nlohmann::json& overrides);

} // namespace Engine
