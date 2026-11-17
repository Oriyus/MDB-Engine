#include "Audio/AudioMix.h"
#include "Core/Log.h"
#include "Core/SettingsFile.h"

#include <nlohmann/json.hpp>
#include <algorithm>

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
        json a;
        if (!ReadSettingsSection(file, "audio", a) || !a.is_object()) return false;
        try {
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
        json a = { { "master", mix.Master }, { "masterMuted", mix.MasterMuted } };
        for (int i = 0; i < kAudioBusCount; i++)
            a["buses"][kNames[i]] = { { "volume", mix.Volume[i] }, { "muted", mix.Muted[i] } };
        return WriteSettingsSection(file, "audio", a);
    }

} // namespace Engine
