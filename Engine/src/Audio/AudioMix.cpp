#include "Audio/AudioMix.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace Engine {

    namespace {
        const char* kNames[kAudioBusCount] = { "sfx", "music", "voice", "ui", "ambience" };
        const char* kLabels[kAudioBusCount] = { "Sound effects", "Music", "Voice", "Interface", "Ambience" };
    }

    const char* AudioBusName(AudioBus bus) { return (int)bus < kAudioBusCount ? kNames[(int)bus] : "sfx"; }
    const char* AudioBusLabel(AudioBus bus) { return (int)bus < kAudioBusCount ? kLabels[(int)bus] : "Sound effects"; }

    bool AudioBusFromName(const std::string& name, AudioBus& out) {
        for (int i = 0; i < kAudioBusCount; i++)
            if (name == kNames[i]) { out = (AudioBus)i; return true; }
        return false;
    }

    bool AudioMix::operator==(const AudioMix& o) const {
        if (Master != o.Master || MasterMuted != o.MasterMuted) return false;
        for (int i = 0; i < kAudioBusCount; i++)
            if (Volume[i] != o.Volume[i] || Muted[i] != o.Muted[i]) return false;
        return true;
    }

    bool LoadAudioMix(const std::string& file, AudioMix& mix) {
        std::ifstream in(fs::u8path(file));
        if (!in) return false;
        try {
            json j;
            in >> j;
            if (!j.is_object() || !j.contains("audio") || !j["audio"].is_object()) return false;
            const json& a = j["audio"];
            AudioMix m;
            m.Master = std::clamp(a.value("master", 1.0f), 0.0f, 1.0f);
            m.MasterMuted = a.value("masterMuted", false);
            if (a.contains("buses") && a["buses"].is_object())
                for (int i = 0; i < kAudioBusCount; i++) {
                    if (!a["buses"].contains(kNames[i]) || !a["buses"][kNames[i]].is_object()) continue;
                    const json& b = a["buses"][kNames[i]];
                    m.Volume[i] = std::clamp(b.value("volume", 1.0f), 0.0f, 1.0f);
                    m.Muted[i] = b.value("muted", false);
                }
            mix = m;
            return true;
        }
        catch (const std::exception& e) {
            ENGINE_WARN("Audio settings '{}': {}", file, e.what());
            return false;
        }
    }

    bool SaveAudioMix(const std::string& file, const AudioMix& mix) {
        const fs::path path = fs::u8path(file);
        json j = json::object();
        {   // keep whatever else the file holds
            std::ifstream in(path);
            if (in) { try { in >> j; } catch (...) { j = json::object(); } }
            if (!j.is_object()) j = json::object();
        }
        json a = { { "master", mix.Master }, { "masterMuted", mix.MasterMuted } };
        for (int i = 0; i < kAudioBusCount; i++)
            a["buses"][kNames[i]] = { { "volume", mix.Volume[i] }, { "muted", mix.Muted[i] } };
        j["audio"] = a;

        std::error_code ec;
        if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
        const fs::path tmp = path.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out) { ENGINE_WARN("Audio settings: can't write '{}'", file); return false; }
            out << j.dump(2);
            if (!out) return false;
        }
        fs::rename(tmp, path, ec);
        if (ec) { ENGINE_WARN("Audio settings: can't replace '{}': {}", file, ec.message()); fs::remove(tmp, ec); return false; }
        return true;
    }

} // namespace Engine
