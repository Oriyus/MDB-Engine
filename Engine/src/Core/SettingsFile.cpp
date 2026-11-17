#include "Core/SettingsFile.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace Engine {

    bool ReadSettingsSection(const std::string& file, const char* key, json& out) {
        std::ifstream in(fs::u8path(file));
        if (!in) return false;
        try {
            json j;
            in >> j;
            if (!j.is_object()) return false;
            auto it = j.find(key);
            if (it == j.end()) return false;
            out = *it;
            return true;
        }
        catch (const std::exception& e) {
            ENGINE_WARN("Settings '{}': {}", file, e.what());
            return false;
        }
    }

    bool WriteSettingsSection(const std::string& file, const char* key, const json& value) {
        const fs::path path = fs::u8path(file);
        json j = json::object();
        {   // keep whatever else the file holds
            std::ifstream in(path);
            if (in) { try { in >> j; } catch (...) { j = json::object(); } }
            if (!j.is_object()) j = json::object();
        }
        if (value.is_null()) j.erase(key);
        else j[key] = value;

        std::error_code ec;
        if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
        const fs::path tmp = path.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out) { ENGINE_WARN("Settings: can't write '{}'", file); return false; }
            out << j.dump(2);
            if (!out) return false;
        }
        fs::rename(tmp, path, ec);
        if (ec) { ENGINE_WARN("Settings: can't replace '{}': {}", file, ec.message()); fs::remove(tmp, ec); return false; }
        return true;
    }

} // namespace Engine
