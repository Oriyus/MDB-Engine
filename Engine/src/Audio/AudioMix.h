#pragma once
#include <cstdint>
#include <string>

namespace Engine {

    // Every sound plays on one bus. Each bus has its own volume and mute, under the master volume.
    // Names (scene files, Lua): "sfx" (the default), "music", "voice", "ui", "ambience"; "master" is the master.
    enum class AudioBus : uint8_t { SFX, Music, Voice, UI, Ambience, Count };
    constexpr int kAudioBusCount = (int)AudioBus::Count;

    const char* AudioBusName(AudioBus bus);          // "sfx", "music" ...
    const char* AudioBusLabel(AudioBus bus);         // "Sound effects", "Music" ... (menus)
    bool AudioBusFromName(const std::string& name, AudioBus& out);   // false for unknown names (and "master")

    // The player's volume settings: master plus one volume and mute per bus. Volumes 0..1.
    struct AudioMix {
        float Master = 1.0f;
        bool  MasterMuted = false;
        float Volume[kAudioBusCount] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        bool  Muted[kAudioBusCount] = {};

        bool operator==(const AudioMix& o) const;
        bool operator!=(const AudioMix& o) const { return !(*this == o); }
    };

    // A settings file holding { "audio": { "master": 1, "masterMuted": false, "buses": { "music": { "volume", "muted" } } } }.
    // Load keeps the defaults for whatever is missing, and false if the file can't be read (mix unchanged).
    // Save keeps the file's other top-level keys, so other settings can share it.
    bool LoadAudioMix(const std::string& file, AudioMix& mix);
    bool SaveAudioMix(const std::string& file, const AudioMix& mix);

} // namespace Engine
