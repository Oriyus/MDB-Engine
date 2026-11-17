#pragma once
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace Engine {

    // A player's settings file is one JSON object whose top-level keys belong to different systems ("audio":
    // AudioMix, "input": binding overrides). Each reads and writes only its own key.

    // False if the file can't be read or parsed, or has no such key (out unchanged)
    bool ReadSettingsSection(const std::string& file, const char* key, nlohmann::json& out);
    // Keeps the file's other keys; writes a temp file, then renames it over the old one. A null value removes the key.
    bool WriteSettingsSection(const std::string& file, const char* key, const nlohmann::json& value);

} // namespace Engine
