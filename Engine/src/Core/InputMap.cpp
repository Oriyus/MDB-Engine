#include "Core/InputMap.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace Engine {

    // ---- dead zones

    static float Rescale(float v, float dead) {
        if (v <= dead) return 0.0f;
        return std::min((v - dead) / std::max(1.0f - dead, 1e-4f), 1.0f);
    }

    static void StickDeadZone(float& x, float& y, float dead) {
        const float len = std::sqrt(x * x + y * y);
        if (len <= dead || len < 1e-6f) { x = y = 0.0f; return; }
        const float k = Rescale(len, dead) / len;        // keep the direction, rescale the tilt
        x *= k; y *= k;
    }

    void ApplyGamepadDeadZones(GamepadState& pad, float stick, float trigger) {
        StickDeadZone(pad.Axes[Pad::LeftX], pad.Axes[Pad::LeftY], stick);
        StickDeadZone(pad.Axes[Pad::RightX], pad.Axes[Pad::RightY], stick);
        pad.Axes[Pad::LT] = Rescale(std::clamp(pad.Axes[Pad::LT], 0.0f, 1.0f), trigger);
        pad.Axes[Pad::RT] = Rescale(std::clamp(pad.Axes[Pad::RT], 0.0f, 1.0f), trigger);
    }

    // ---- names

    namespace {
        struct KeyEntry { const char* Name; int Code; };
        const KeyEntry kKeys[] = {
            { "Space", 32 }, { "Apostrophe", 39 }, { "Comma", 44 }, { "Minus", 45 }, { "Period", 46 }, { "Slash", 47 },
            { "Semicolon", 59 }, { "Equal", 61 }, { "LeftBracket", 91 }, { "Backslash", 92 }, { "RightBracket", 93 },
            { "Grave", 96 }, { "Escape", 256 }, { "Enter", 257 }, { "Tab", 258 }, { "Backspace", 259 },
            { "Insert", 260 }, { "Delete", 261 }, { "Right", 262 }, { "Left", 263 }, { "Down", 264 }, { "Up", 265 },
            { "PageUp", 266 }, { "PageDown", 267 }, { "Home", 268 }, { "End", 269 },
            { "LeftShift", 340 }, { "LeftCtrl", 341 }, { "LeftAlt", 342 },
            { "RightShift", 344 }, { "RightCtrl", 345 }, { "RightAlt", 346 },
        };
        const char* kPadButtons[Pad::ButtonCount] = {
            "A", "B", "X", "Y", "LB", "RB", "Back", "Start", "Guide", "LS", "RS", "DPadUp", "DPadRight", "DPadDown", "DPadLeft" };
        const char* kPadAxes[Pad::AxisCount] = { "LeftX", "LeftY", "RightX", "RightY", "LT", "RT" };
        // Half axes: the name, the axis and the sign
        struct HalfAxis { const char* Name; int Axis; float Sign; };
        const HalfAxis kHalves[] = {
            { "LeftUp", Pad::LeftY, 1 }, { "LeftDown", Pad::LeftY, -1 }, { "LeftLeft", Pad::LeftX, -1 }, { "LeftRight", Pad::LeftX, 1 },
            { "RightUp", Pad::RightY, 1 }, { "RightDown", Pad::RightY, -1 }, { "RightLeft", Pad::RightX, -1 }, { "RightRight", Pad::RightX, 1 },
            { "LT", Pad::LT, 1 }, { "RT", Pad::RT, 1 },
        };
    }

    int KeyFromName(const std::string& name) {
        if (name.size() == 1) {
            char c = name[0];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;     // GLFW uses ASCII for these
        }
        if (name.size() >= 2 && name.size() <= 3 && name[0] == 'F' && std::isdigit((unsigned char)name[1])) {
            const int n = std::atoi(name.c_str() + 1);
            if (n >= 1 && n <= 12) return 289 + n;
        }
        if (name.size() == 3 && name.rfind("KP", 0) == 0 && std::isdigit((unsigned char)name[2])) return 320 + (name[2] - '0');
        for (const auto& k : kKeys) if (name == k.Name) return k.Code;
        return -1;
    }

    const char* KeyName(int key) {
        static thread_local char buf[8];
        if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) { buf[0] = (char)key; buf[1] = 0; return buf; }
        if (key >= 290 && key <= 301) { std::snprintf(buf, sizeof buf, "F%d", key - 289); return buf; }
        if (key >= 320 && key <= 329) { std::snprintf(buf, sizeof buf, "KP%d", key - 320); return buf; }
        for (const auto& k : kKeys) if (k.Code == key) return k.Name;
        return nullptr;
    }

    int PadButtonFromName(const std::string& name) {
        for (int b = 0; b < Pad::ButtonCount; b++) if (name == kPadButtons[b]) return b;
        return -1;
    }
    int PadAxisFromName(const std::string& name) {
        for (int a = 0; a < Pad::AxisCount; a++) if (name == kPadAxes[a]) return a;
        return -1;
    }
    const char* PadButtonName(int b) { return b >= 0 && b < Pad::ButtonCount ? kPadButtons[b] : "?"; }
    const char* PadAxisName(int a) { return a >= 0 && a < Pad::AxisCount ? kPadAxes[a] : "?"; }

    const char* InputActionTypeName(InputActionType t) {
        switch (t) {
            case InputActionType::Axis: return "axis";
            case InputActionType::Vector: return "vector";
            default: return "button";
        }
    }

    bool ParseInputSource(const std::string& text, InputSource& out) {
        const size_t colon = text.find(':');
        if (colon == std::string::npos) return false;
        const std::string dev = text.substr(0, colon), name = text.substr(colon + 1);
        if (dev == "key") {
            const int k = KeyFromName(name);
            if (k < 0) return false;
            out = { InputSource::Kind::Key, k, 1.0f };
            return true;
        }
        if (dev == "mouse") {
            const int b = name == "Left" ? 0 : name == "Right" ? 1 : name == "Middle" ? 2 : -1;
            if (b < 0) return false;
            out = { InputSource::Kind::Mouse, b, 1.0f };
            return true;
        }
        if (dev == "pad") {
            if (int b = PadButtonFromName(name); b >= 0) { out = { InputSource::Kind::PadButton, b, 1.0f }; return true; }
            for (const auto& h : kHalves)
                if (name == h.Name) { out = { InputSource::Kind::PadAxis, h.Axis, h.Sign }; return true; }
        }
        return false;
    }

    std::string InputSourceName(const InputSource& s) {
        switch (s.K) {
            case InputSource::Kind::Key: { const char* n = KeyName(s.Code); return std::string("key:") + (n ? n : "?"); }
            case InputSource::Kind::Mouse: return std::string("mouse:") + (s.Code == 0 ? "Left" : s.Code == 1 ? "Right" : "Middle");
            case InputSource::Kind::PadButton: return std::string("pad:") + PadButtonName(s.Code);
            case InputSource::Kind::PadAxis:
                for (const auto& h : kHalves) if (h.Axis == s.Code && h.Sign == s.Sign) return std::string("pad:") + h.Name;
                return "pad:?";
        }
        return "?";
    }

    // ---- loading

    void InputMap::Clear() {
        m_Actions.clear();
        m_Index.clear();
        m_HasFile = false;
        StickDeadZone = 0.2f;
        TriggerDeadZone = 0.1f;
    }

    // Groups usable in a vector's "bindings": up, down, left, right
    static bool VectorGroup(const std::string& text, std::array<const char*, 4>& out) {
        if (text == "key:WASD")       { out = { "key:W", "key:S", "key:A", "key:D" }; return true; }
        if (text == "key:Arrows")     { out = { "key:Up", "key:Down", "key:Left", "key:Right" }; return true; }
        if (text == "pad:LeftStick")  { out = { "pad:LeftUp", "pad:LeftDown", "pad:LeftLeft", "pad:LeftRight" }; return true; }
        if (text == "pad:RightStick") { out = { "pad:RightUp", "pad:RightDown", "pad:RightLeft", "pad:RightRight" }; return true; }
        if (text == "pad:DPad")       { out = { "pad:DPadUp", "pad:DPadDown", "pad:DPadLeft", "pad:DPadRight" }; return true; }
        return false;
    }

    bool InputMap::Load(const nlohmann::json& j, std::vector<std::string>& warnings) {
        Clear();
        if (!j.is_object()) { warnings.push_back("the file must hold a JSON object"); return false; }
        if (auto it = j.find("deadzone"); it != j.end() && it->is_number())
            StickDeadZone = std::clamp(it->get<float>(), 0.0f, 0.9f);
        if (auto it = j.find("triggerDeadzone"); it != j.end() && it->is_number())
            TriggerDeadZone = std::clamp(it->get<float>(), 0.0f, 0.9f);

        auto acts = j.find("actions");
        if (acts == j.end()) return true;
        if (!acts->is_object()) { warnings.push_back("\"actions\" must be an object"); return true; }

        for (auto it = acts->begin(); it != acts->end(); ++it) {
            const std::string& name = it.key();
            InputAction a;
            a.Name = name;
            const nlohmann::json& def = it.value();
            // slot -1: a vector's "bindings", which take groups only
            auto addList = [&](const nlohmann::json* list, int slot, const char* what) {
                if (!list) return;
                if (!list->is_array()) { warnings.push_back("action '" + name + "': \"" + what + "\" must be a list"); return; }
                for (const auto& s : *list) {
                    if (!s.is_string()) { warnings.push_back("action '" + name + "': a binding is not a string"); continue; }
                    const std::string text = s.get<std::string>();
                    if (slot < 0) {
                        std::array<const char*, 4> group;
                        if (VectorGroup(text, group)) {
                            for (int k = 0; k < 4; k++) { InputSource src; ParseInputSource(group[k], src); a.Slots[k].push_back(src); }
                        }
                        else warnings.push_back("action '" + name + "': \"" + text +
                            "\" is not a group (key:WASD, key:Arrows, pad:LeftStick, pad:RightStick, pad:DPad)");
                        continue;
                    }
                    InputSource src;
                    if (ParseInputSource(text, src)) a.Slots[slot].push_back(src);
                    else warnings.push_back("action '" + name + "': unknown binding \"" + text + "\"");
                }
            };
            auto field = [&](const char* key) -> const nlohmann::json* {
                auto f = def.find(key);
                return f == def.end() ? nullptr : &*f;
            };

            if (def.is_array()) {                                            // short form: a button
                addList(&def, 0, "bindings");
            }
            else if (def.is_object()) {
                const std::string type = def.value("type", std::string("button"));
                if (type == "button") {
                    a.Type = InputActionType::Button;
                    addList(field("bindings"), 0, "bindings");
                }
                else if (type == "axis") {
                    a.Type = InputActionType::Axis;
                    addList(field("positive"), 0, "positive");
                    addList(field("negative"), 1, "negative");
                }
                else if (type == "vector") {
                    a.Type = InputActionType::Vector;
                    addList(field("bindings"), -1, "bindings");
                    addList(field("up"), 0, "up");
                    addList(field("down"), 1, "down");
                    addList(field("left"), 2, "left");
                    addList(field("right"), 3, "right");
                }
                else { warnings.push_back("action '" + name + "': unknown type \"" + type + "\" (button, axis, vector)"); continue; }
            }
            else { warnings.push_back("action '" + name + "': must be a list of bindings or an object"); continue; }
            m_Actions.push_back(std::move(a));
        }
        std::sort(m_Actions.begin(), m_Actions.end(), [](const InputAction& x, const InputAction& y) { return x.Name < y.Name; });
        for (size_t i = 0; i < m_Actions.size(); i++) m_Index[m_Actions[i].Name] = i;
        return true;
    }

    bool InputMap::LoadFile(const std::string& file, std::string& error, std::vector<std::string>& warnings) {
        Clear();
        std::error_code ec;
        const auto path = std::filesystem::u8path(file);
        if (!std::filesystem::is_regular_file(path, ec)) return true;
        std::ifstream in(path);
        nlohmann::json j;
        try { in >> j; }
        catch (const std::exception& ex) { error = file + ": " + ex.what(); return false; }
        const size_t first = warnings.size();
        Load(j, warnings);
        for (size_t i = first; i < warnings.size(); i++) warnings[i] = file + ": " + warnings[i];
        m_HasFile = true;
        return true;
    }

    // ---- per frame

    static float SourceValue(const InputSource& s, const InputFrame& f) {
        switch (s.K) {
            case InputSource::Kind::Key:   return s.Code >= 0 && s.Code < f.KeyCount && f.Keys[s.Code] ? 1.0f : 0.0f;
            case InputSource::Kind::Mouse: return s.Code >= 0 && s.Code < f.MouseCount && f.Mouse[s.Code] ? 1.0f : 0.0f;
            case InputSource::Kind::PadButton: return f.Pad && f.Pad->Connected && f.Pad->Buttons[s.Code] ? 1.0f : 0.0f;
            case InputSource::Kind::PadAxis:
                return f.Pad && f.Pad->Connected ? std::max(0.0f, f.Pad->Axes[s.Code] * s.Sign) : 0.0f;
        }
        return 0.0f;
    }

    static float Strongest(const std::vector<InputSource>& list, const InputFrame& f) {
        float v = 0.0f;
        for (const auto& s : list) v = std::max(v, SourceValue(s, f));
        return v;
    }

    void InputMap::Update(const InputFrame& f) {
        for (auto& a : m_Actions) {
            a.WasDown = a.Down;
            switch (a.Type) {
                case InputActionType::Button:
                    a.X = Strongest(a.Slots[0], f); a.Y = 0.0f;
                    a.Strength = a.X;
                    break;
                case InputActionType::Axis:
                    a.X = Strongest(a.Slots[0], f) - Strongest(a.Slots[1], f); a.Y = 0.0f;
                    a.Strength = std::abs(a.X);
                    break;
                case InputActionType::Vector: {
                    a.X = Strongest(a.Slots[3], f) - Strongest(a.Slots[2], f);
                    a.Y = Strongest(a.Slots[0], f) - Strongest(a.Slots[1], f);
                    const float len = std::sqrt(a.X * a.X + a.Y * a.Y);
                    if (len > 1.0f) { a.X /= len; a.Y /= len; }              // diagonals on keys: not faster
                    a.Strength = std::min(len, 1.0f);
                    break;
                }
            }
            a.Down = a.Strength >= (a.WasDown ? 0.35f : 0.5f);
        }
    }

    const InputAction* InputMap::Find(const std::string& name) const {
        auto it = m_Index.find(name);
        return it == m_Index.end() ? nullptr : &m_Actions[it->second];
    }

    std::string InputMap::Names() const {
        std::string s;
        for (const auto& a : m_Actions) { if (!s.empty()) s += ", "; s += a.Name; }
        return s;
    }

    const char* InputMap::DefaultJson() {
        return R"JSON({
  "deadzone": 0.2,
  "triggerDeadzone": 0.1,
  "actions": {
    "move":     { "type": "vector", "bindings": ["key:WASD", "key:Arrows", "pad:LeftStick", "pad:DPad"] },
    "look":     { "type": "vector", "bindings": ["pad:RightStick"] },
    "jump":     ["key:Space", "pad:A"],
    "sprint":   ["key:LeftShift", "key:RightShift", "pad:LS", "pad:LT"],
    "interact": ["key:E", "pad:X"],
    "confirm":  ["key:Enter", "key:Space", "pad:A"],
    "cancel":   ["key:Escape", "key:Backspace", "pad:B"],
    "pause":    ["pad:Start"]
  }
}
)JSON";
    }

} // namespace Engine
