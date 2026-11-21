#pragma once
#include <string>

namespace Engine {

    // The player's video preferences: the Game app applies them (window, renderer) and scripts read and change them
    // through video.* (Scripting/ScriptEngine.h). Quality switches only turn OFF what a level has on: a level
    // without volumetric fog stays without it when the switch is on. Brightness multiplies the level's exposure.
    struct VideoSettings {
        bool  Fullscreen = false;
        bool  VSync = true;
        bool  Shadows = true;
        bool  AmbientOcclusion = true;
        bool  Bloom = true;
        bool  VolumetricFog = true;
        int   AntiAliasing = 2;          // 0 off, 1 FXAA, 2 TAA
        float Brightness = 1.0f;         // 0.5 .. 2

        bool operator==(const VideoSettings& o) const = default;
    };

    // A settings file's "video" key (Core/SettingsFile.h):
    // { "fullscreen", "vsync", "shadows", "ambientOcclusion", "bloom", "volumetricFog", "antiAliasing": "taa", "brightness" }
    // Load keeps the defaults for whatever is missing, and is false if there is no such section (video unchanged).
    bool LoadVideoSettings(const std::string& file, VideoSettings& video);
    bool SaveVideoSettings(const std::string& file, const VideoSettings& video);

    const char* AntiAliasingName(int mode);              // "off" "fxaa" "taa"
    int AntiAliasingFromName(const std::string& name);   // -1 if unknown

} // namespace Engine
