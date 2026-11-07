#include "Audio/AudioEngine.h"
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Core/Log.h"

// miniaudio's implementation lives in this file only. It pulls in <windows.h>.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Engine {

    namespace {
        constexpr size_t kMaxOneShots = 64;   // beyond this, new one-shots are skipped
    }

    // A ma_sound must not move after init, so every voice lives on the heap
    struct Voice {
        ma_sound Sound{};
        bool Initialized = false;
        ~Voice() { if (Initialized) ma_sound_uninit(&Sound); }
    };

    struct Source {
        std::unique_ptr<Voice> V;
        std::string Clip;                     // what V was made from (recreate when it changes)
    };

    struct AudioEngine::Impl {
        std::string AssetDir;
        ma_engine Engine{};
        bool Ready = false;
        bool Paused = false;
        float MasterVolume = 1.0f;

        std::vector<std::unique_ptr<Voice>> OneShots;
        std::unordered_map<entt::entity, Source> Sources;
        std::unordered_set<entt::entity> PendingPlay;     // PlaySource before the source exists
        std::unordered_set<std::string> Missing;          // clips that failed to load: warn once

        std::unique_ptr<Voice> Load(const std::string& clip, bool spatial) {
            if (!Ready || clip.empty() || Missing.count(clip)) return nullptr;
            auto v = std::make_unique<Voice>();
            const ma_uint32 flags = MA_SOUND_FLAG_DECODE | (spatial ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION);
            const std::string path = AssetDir + clip;
            if (ma_sound_init_from_file(&Engine, path.c_str(), flags, nullptr, nullptr, &v->Sound) != MA_SUCCESS) {
                if (Missing.insert(clip).second) ENGINE_WARN("Audio: can't load '{}'", path);
                return nullptr;
            }
            v->Initialized = true;
            return v;
        }

        void ReapOneShots() {
            OneShots.erase(std::remove_if(OneShots.begin(), OneShots.end(),
                [](const std::unique_ptr<Voice>& v) { return ma_sound_at_end(&v->Sound); }), OneShots.end());
        }

        void StartOneShot(std::unique_ptr<Voice> v, float volume, float pitch) {
            ma_sound_set_volume(&v->Sound, std::max(volume, 0.0f));
            ma_sound_set_pitch(&v->Sound, std::max(pitch, 0.01f));
            ma_sound_start(&v->Sound);
            OneShots.push_back(std::move(v));
        }

        static void Apply(ma_sound* s, const AudioSourceComponent& a, const glm::vec3& pos) {
            ma_sound_set_volume(s, std::max(a.Volume, 0.0f));
            ma_sound_set_pitch(s, std::max(a.Pitch, 0.01f));
            ma_sound_set_looping(s, a.Loop ? MA_TRUE : MA_FALSE);
            ma_sound_set_spatialization_enabled(s, a.Spatial ? MA_TRUE : MA_FALSE);
            ma_sound_set_position(s, pos.x, pos.y, pos.z);
            ma_sound_set_min_distance(s, std::max(a.MinDistance, 0.01f));
            ma_sound_set_max_distance(s, std::max(a.MaxDistance, a.MinDistance + 0.01f));
        }
    };

    AudioEngine::AudioEngine(std::string assetDir) : m_Impl(std::make_unique<Impl>()) {
        m_Impl->AssetDir = std::move(assetDir);
        if (ma_engine_init(nullptr, &m_Impl->Engine) == MA_SUCCESS) {
            m_Impl->Ready = true;
            ENGINE_INFO("Audio ready (miniaudio, {} Hz)", ma_engine_get_sample_rate(&m_Impl->Engine));
        }
        else {
            ENGINE_WARN("Audio: no output device, sound is off");
        }
    }

    AudioEngine::~AudioEngine() {
        // Voices first: they must be uninitialised before the engine they play on
        m_Impl->OneShots.clear();
        m_Impl->Sources.clear();
        if (m_Impl->Ready) ma_engine_uninit(&m_Impl->Engine);
    }

    bool AudioEngine::IsReady() const { return m_Impl->Ready; }

    void AudioEngine::Play(const std::string& clip, float volume, float pitch) {
        Impl& I = *m_Impl;
        I.ReapOneShots();
        if (I.OneShots.size() >= kMaxOneShots) return;
        if (auto v = I.Load(clip, false)) I.StartOneShot(std::move(v), volume, pitch);
    }

    void AudioEngine::PlayAt(const std::string& clip, const glm::vec3& position, float volume, float pitch) {
        Impl& I = *m_Impl;
        I.ReapOneShots();
        if (I.OneShots.size() >= kMaxOneShots) return;
        if (auto v = I.Load(clip, true)) {
            ma_sound_set_position(&v->Sound, position.x, position.y, position.z);
            ma_sound_set_min_distance(&v->Sound, 2.0f);       // one-shots carry well: full volume nearby
            ma_sound_set_max_distance(&v->Sound, 40.0f);
            I.StartOneShot(std::move(v), volume, pitch);
        }
    }

    void AudioEngine::Update(Scene& scene) {
        Impl& I = *m_Impl;
        if (!I.Ready) return;
        I.ReapOneShots();
        auto& reg = scene.Registry();

        for (auto it = I.Sources.begin(); it != I.Sources.end();) {
            if (!reg.valid(it->first) || !reg.all_of<AudioSourceComponent>(it->first)) it = I.Sources.erase(it);
            else ++it;
        }

        for (auto [e, t, a] : reg.view<TransformComponent, AudioSourceComponent>().each()) {
            Source& src = I.Sources[e];
            const bool fresh = !src.V || src.Clip != a.Clip;
            if (fresh) {
                src.V = I.Load(a.Clip, true);   // spatialisation is switched per frame in Apply
                src.Clip = a.Clip;
                if (!src.V) continue;
            }
            Impl::Apply(&src.V->Sound, a, glm::vec3(scene.GetWorldMatrix(e)[3]));   // world position (parents included)
            if (fresh && (a.PlayOnStart || I.PendingPlay.count(e))) ma_sound_start(&src.V->Sound);
            else if (I.PendingPlay.count(e)) { ma_sound_seek_to_pcm_frame(&src.V->Sound, 0); ma_sound_start(&src.V->Sound); }
        }
        I.PendingPlay.clear();
    }

    void AudioEngine::PlaySource(entt::entity e) { m_Impl->PendingPlay.insert(e); }   // applied by Update

    void AudioEngine::StopSource(entt::entity e) {
        m_Impl->PendingPlay.erase(e);
        auto it = m_Impl->Sources.find(e);
        if (it != m_Impl->Sources.end() && it->second.V) {
            ma_sound_stop(&it->second.V->Sound);
            ma_sound_seek_to_pcm_frame(&it->second.V->Sound, 0);
        }
    }

    bool AudioEngine::IsSourcePlaying(entt::entity e) const {
        if (m_Impl->PendingPlay.count(e)) return true;
        auto it = m_Impl->Sources.find(e);
        return it != m_Impl->Sources.end() && it->second.V && ma_sound_is_playing(&it->second.V->Sound);
    }

    void AudioEngine::StopAll() {
        m_Impl->OneShots.clear();
        m_Impl->Sources.clear();
        m_Impl->PendingPlay.clear();
        SetPaused(false);
    }

    void AudioEngine::SetPaused(bool paused) {
        Impl& I = *m_Impl;
        if (!I.Ready || paused == I.Paused) return;
        I.Paused = paused;
        if (paused) ma_engine_stop(&I.Engine);
        else        ma_engine_start(&I.Engine);
    }

    void AudioEngine::SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) {
        if (!m_Impl->Ready) return;
        ma_engine_listener_set_position(&m_Impl->Engine, 0, position.x, position.y, position.z);
        ma_engine_listener_set_direction(&m_Impl->Engine, 0, forward.x, forward.y, forward.z);
        ma_engine_listener_set_world_up(&m_Impl->Engine, 0, up.x, up.y, up.z);
    }

    void AudioEngine::SetMasterVolume(float volume) {
        m_Impl->MasterVolume = std::clamp(volume, 0.0f, 1.0f);
        if (m_Impl->Ready) ma_engine_set_volume(&m_Impl->Engine, m_Impl->MasterVolume);
    }

    float AudioEngine::GetMasterVolume() const { return m_Impl->MasterVolume; }

    uint32_t AudioEngine::VoiceCount() const {
        return (uint32_t)(m_Impl->OneShots.size() + m_Impl->Sources.size());
    }

} // namespace Engine
