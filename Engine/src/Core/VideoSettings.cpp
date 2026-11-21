#include "Core/VideoSettings.h"
#include "Core/SettingsFile.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>
#include <algorithm>

namespace Engine {

    using json = nlohmann::json;

    const char* AntiAliasingName(int mode) {
        return mode == 0 ? "off" : (mode == 1 ? "fxaa" : "taa");
    }

    int AntiAliasingFromName(const std::string& name) {
        if (name == "off") return 0;
        if (name == "fxaa") return 1;
        if (name == "taa") return 2;
        return -1;
    }

    bool LoadVideoSettings(const std::string& file, VideoSettings& video) {
        json v;
        if (!ReadSettingsSection(file, "video", v) || !v.is_object()) return false;
        try {
            VideoSettings s = video;
            s.Fullscreen = v.value("fullscreen", s.Fullscreen);
            s.VSync = v.value("vsync", s.VSync);
            s.Shadows = v.value("shadows", s.Shadows);
            s.AmbientOcclusion = v.value("ambientOcclusion", s.AmbientOcclusion);
            s.Bloom = v.value("bloom", s.Bloom);
            s.VolumetricFog = v.value("volumetricFog", s.VolumetricFog);
            const int aa = AntiAliasingFromName(v.value("antiAliasing", std::string(AntiAliasingName(s.AntiAliasing))));
            if (aa >= 0) s.AntiAliasing = aa;
            s.Brightness = std::clamp(v.value("brightness", s.Brightness), 0.5f, 2.0f);
            video = s;
            return true;
        }
        catch (const std::exception& e) {
            ENGINE_WARN("Video settings '{}': {}", file, e.what());
            return false;
        }
    }

    bool SaveVideoSettings(const std::string& file, const VideoSettings& video) {
        const json v = {
            { "fullscreen", video.Fullscreen }, { "vsync", video.VSync }, { "shadows", video.Shadows },
            { "ambientOcclusion", video.AmbientOcclusion }, { "bloom", video.Bloom },
            { "volumetricFog", video.VolumetricFog }, { "antiAliasing", AntiAliasingName(video.AntiAliasing) },
            { "brightness", video.Brightness },
        };
        return WriteSettingsSection(file, "video", v);
    }

} // namespace Engine
