#include "Runtime/UIPreview.h"

#include <sol/sol.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <unordered_map>

namespace Engine {

    struct UIPreview::Impl {
        std::unique_ptr<sol::state> Lua;
        sol::table Env;
        std::unordered_map<std::string, sol::protected_function> Code;   // "e:" / "s:" + code (invalid = doesn't compile)
        struct Frame { sol::object Index, Item; sol::table List; };
        std::vector<Frame> Stack;
    };

    namespace {
        // Point a loaded chunk's _ENV (its first upvalue) at a table
        void SetChunkEnv(const sol::protected_function& f, const sol::table& env) {
            lua_State* L = f.lua_state();
            f.push();
            env.push();
            if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1);
            lua_pop(L, 1);
        }

        sol::object FromJson(sol::state& lua, const nlohmann::json& j) {
            switch (j.type()) {
            case nlohmann::json::value_t::boolean: return sol::make_object(lua, j.get<bool>());
            case nlohmann::json::value_t::number_integer:
            case nlohmann::json::value_t::number_unsigned: return sol::make_object(lua, j.get<long long>());
            case nlohmann::json::value_t::number_float: return sol::make_object(lua, j.get<double>());
            case nlohmann::json::value_t::string: return sol::make_object(lua, j.get<std::string>());
            case nlohmann::json::value_t::array: {
                sol::table t = lua.create_table();
                int i = 1;
                for (const auto& v : j) t[i++] = FromJson(lua, v);
                return t;
            }
            case nlohmann::json::value_t::object: {
                sol::table t = lua.create_table();
                for (auto it = j.begin(); it != j.end(); ++it) t[it.key()] = FromJson(lua, it.value());
                return t;
            }
            default: return sol::make_object(lua, sol::lua_nil);
            }
        }

        std::string ToText(sol::state& lua, const sol::object& v) {
            switch (v.get_type()) {
            case sol::type::lua_nil: case sol::type::none: return "";
            case sol::type::number: {
                const double d = v.as<double>();
                char buf[64];
                if (std::floor(d) == d && std::abs(d) < 1e15) std::snprintf(buf, sizeof buf, "%lld", (long long)d);
                else std::snprintf(buf, sizeof buf, "%g", d);
                return buf;
            }
            case sol::type::string: return v.as<std::string>();
            case sol::type::boolean: return v.as<bool>() ? "true" : "false";
            default: {
                sol::protected_function ts = lua["tostring"];
                auto r = ts(v);
                return r.valid() ? r.get<std::string>() : "?";
            }
            }
        }
    }

    UIPreview::UIPreview() : m(std::make_unique<Impl>()) { Reset(); }
    UIPreview::~UIPreview() = default;

    void UIPreview::SetVars(const nlohmann::json& vars) {
        std::string text = vars.dump();
        if (text == m_VarsText) return;
        m_VarsText = std::move(text);
        m_Vars = vars;
        Reset();
    }

    void UIPreview::Reset() {
        m->Code.clear();
        m->Stack.clear();
        m->Env = sol::table();
        m->Lua = std::make_unique<sol::state>();
        sol::state& lua = *m->Lua;
        lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);
        m->Env = lua.create_table();
        sol::table meta = lua.create_table();
        meta["__index"] = lua.globals();
        m->Env[sol::metatable_key] = meta;
        if (m_Vars.is_object())
            for (auto it = m_Vars.begin(); it != m_Vars.end(); ++it) m->Env[it.key()] = FromJson(lua, it.value());
        Ran.clear();
    }

    void UIPreview::BeginFrame() { m_Errors.clear(); }

    // nullopt: it failed (noted in errors); else the value (an invalid reference is nil)
    static std::optional<sol::object> Eval(UIPreview::Impl& m, std::vector<std::string>& errors, const std::string& code, bool expression) {
        sol::state& lua = *m.Lua;
        const std::string key = (expression ? "e:" : "s:") + code;
        auto note = [&](const std::string& e) {
            if (std::find(errors.begin(), errors.end(), e) == errors.end() && errors.size() < 50) errors.push_back(e);
        };
        auto it = m.Code.find(key);
        if (it == m.Code.end()) {
            sol::load_result chunk = lua.load(expression ? "return " + code : code, "=" + code, sol::load_mode::text);
            if (!chunk.valid()) {
                sol::error err = chunk;
                it = m.Code.emplace(key, sol::protected_function()).first;
                note(std::string("doesn't compile: ") + err.what());
                return std::nullopt;
            }
            it = m.Code.emplace(key, chunk.get<sol::protected_function>()).first;
        }
        if (!it->second.valid()) {
            note("doesn't compile: " + code);
            return std::nullopt;
        }
        SetChunkEnv(it->second, m.Env);
        sol::protected_function_result r = it->second();
        if (!r.valid()) {
            sol::error err = r;
            note(err.what());
            return std::nullopt;
        }
        if (r.return_count() == 0) return sol::make_object(lua, sol::lua_nil);
        return r.get<sol::object>(0);
    }

    std::string UIPreview::Text(const std::string& text) {
        std::string out;
        size_t i = 0;
        while (i < text.size()) {
            const size_t start = text.find("{=", i);
            if (start == std::string::npos) { out += text.substr(i); break; }
            const size_t end = text.find('}', start + 2);
            if (end == std::string::npos) { out += text.substr(i); break; }
            out += text.substr(i, start - i);
            const auto v = Eval(*m, m_Errors, text.substr(start + 2, end - start - 2), true);
            out += v ? (v->valid() ? ToText(*m->Lua, *v) : std::string()) : std::string("?");
            i = end + 1;
        }
        return out;
    }

    bool UIPreview::Bool(const std::string& expr, bool fallback) {
        const auto v = Eval(*m, m_Errors, expr, true);
        if (!v) return fallback;
        if (!v->valid()) return false;   // nil
        return v->get_type() != sol::type::lua_nil && v->get_type() != sol::type::none &&
               !(v->get_type() == sol::type::boolean && !v->as<bool>());
    }

    float UIPreview::Number(const std::string& expr, float fallback) {
        const auto v = Eval(*m, m_Errors, expr, true);
        return v && v->valid() && v->get_type() == sol::type::number ? v->as<float>() : fallback;
    }

    int UIPreview::BeginRepeat(const std::string& expr) {
        Impl::Frame f;
        f.Index = m->Env.raw_get<sol::object>("index");
        f.Item = m->Env.raw_get<sol::object>("item");
        const auto v = Eval(*m, m_Errors, expr, true);
        int n = 0;
        if (v && v->valid() && v->get_type() == sol::type::number) n = (int)v->as<double>();
        else if (v && v->valid() && v->get_type() == sol::type::table) { f.List = v->as<sol::table>(); n = (int)f.List.size(); }
        m->Stack.push_back(std::move(f));
        return n;
    }

    void UIPreview::SetRepeat(int index) {
        if (m->Stack.empty()) return;
        const Impl::Frame& f = m->Stack.back();
        m->Env.raw_set("index", index);
        if (f.List.valid()) m->Env.raw_set("item", f.List.raw_get<sol::object>(index));
        else m->Env.raw_set("item", sol::lua_nil);
    }

    void UIPreview::EndRepeat() {
        if (m->Stack.empty()) return;
        m->Env.raw_set("index", m->Stack.back().Index);
        m->Env.raw_set("item", m->Stack.back().Item);
        m->Stack.pop_back();
    }

    void UIPreview::Run(const std::string& code) {
        Ran.push_back(code);
        if (Ran.size() > 20) Ran.erase(Ran.begin());
        if (RunActions) Eval(*m, m_Errors, code, false);
    }

    void UIPreview::Event(const std::string& name, const UINode& node, int index) {
        (void)node; (void)index;
        Ran.push_back("events.emit(\"" + name + "\")");
        if (Ran.size() > 20) Ran.erase(Ran.begin());
    }

} // namespace Engine
