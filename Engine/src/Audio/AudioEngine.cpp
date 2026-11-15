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

        AudioBus ValidBus(AudioBus b) { return (int)b < kAudioBusCount ? b : AudioBus::SFX; }
    }

    // A ma_sound must not move after init, so every voice lives on the heap
    struct Voice {
        ma_sound Sound{};
        bool Initialized = false;
        AudioBus Bus = AudioBus::SFX;
        ~Voice() { if (Initialized) ma_sound_uninit(&Sound); }
    };

    struct Source {
        std::unique_ptr<Voice> V;
        std::string Clip;                     // what V was made from (recreate when it changes)
        AudioBus Bus = AudioBus::SFX;         // the group V plays in (a sound can't change group: recreate)
    };

    struct AudioEngine::Impl {
        std::string AssetDir;
        ma_engine Engine{};
        ma_sound_group Groups[kAudioBusCount]{};   // one per bus, straight into the engine's endpoint
        bool GroupsReady = false;
        bool Ready = false;
        bool Paused = false;
        AudioMix Mix;
        uint32_t Revision = 0;

        std::vector<std::unique_ptr<Voice>> OneShots;
        std::unordered_map<entt::entity, Source> Sources;
        std::unordered_set<entt::entity> PendingPlay;     // PlaySource before the source exists
        std::unordered_set<std::string> Missing;          // clips that failed to load: warn once

        std::unique_ptr<Voice> Load(const std::string& clip, bool spatial, AudioBus bus) {
            if (!Ready || clip.empty() || Missing.count(clip)) return nullptr;
            bus = ValidBus(bus);
            auto v = std::make_unique<Voice>();
            v->Bus = bus;
            const ma_uint32 flags = MA_SOUND_FLAG_DECODE | (spatial ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION);
            const std::string path = AssetDir + clip;
            if (ma_sound_init_from_file(&Engine, path.c_str(), flags, &Groups[(int)bus], nullptr, &v->Sound) != MA_SUCCESS) {
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

        // The mix onto the engine and groups (volumes are 0 while muted)
        void ApplyMix() {
            if (!Ready) return;
            ma_engine_set_volume(&Engine, Mix.MasterMuted ? 0.0f : Mix.Master);
            for (int i = 0; i < kAudioBusCount; i++)
                ma_sound_group_set_volume(&Groups[i], Mix.Muted[i] ? 0.0f : Mix.Volume[i]);
        }

        void SetMix(const AudioMix& m) {
            AudioMix c = m;
            c.Master = std::clamp(c.Master, 0.0f, 1.0f);
            for (float& v : c.Volume) v = std::clamp(v, 0.0f, 1.0f);
            if (c == Mix) return;
            Mix = c;
            Revision++;
            ApplyMix();
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
        Impl& I = *m_Impl;
        I.AssetDir = std::move(assetDir);
        if (ma_engine_init(nullptr, &I.Engine) != MA_SUCCESS) {
            ENGINE_WARN("Audio: no output device, sound is off");
            return;
        }
        int made = 0;
        for (; made < kAudioBusCount; made++)
            if (ma_sound_group_init(&I.Engine, 0, nullptr, &I.Groups[made]) != MA_SUCCESS) break;
        if (made < kAudioBusCount) {
            for (int i = 0; i < made; i++) ma_sound_group_uninit(&I.Groups[i]);
            ma_engine_uninit(&I.Engine);
            ENGINE_WARN("Audio: can't create the mixer buses, sound is off");
            return;
        }
        I.GroupsReady = true;
        I.Ready = true;
        I.ApplyMix();
        ENGINE_INFO("Audio ready (miniaudio, {} Hz, {} buses)", ma_engine_get_sample_rate(&I.Engine), kAudioBusCount);
    }

    AudioEngine::~AudioEngine() {
        // Voices first, then the groups they play in, then the engine
        Impl& I = *m_Impl;
        I.OneShots.clear();
        I.Sources.clear();
        if (I.GroupsReady) for (auto& g : I.Groups) ma_sound_group_uninit(&g);
        if (I.Ready) ma_engine_uninit(&I.Engine);
    }

    bool AudioEngine::IsReady() const { return m_Impl->Ready; }

    void AudioEngine::Play(const std::string& clip, float volume, float pitch, AudioBus bus) {
        Impl& I = *m_Impl;
        I.ReapOneShots();
        if (I.OneShots.size() >= kMaxOneShots) return;
        if (auto v = I.Load(clip, false, bus)) I.StartOneShot(std::move(v), volume, pitch);
    }

    void AudioEngine::PlayAt(const std::string& clip, const glm::vec3& position, float volume, float pitch, AudioBus bus) {
        Impl& I = *m_Impl;
        I.ReapOneShots();
        if (I.OneShots.size() >= kMaxOneShots) return;
        if (auto v = I.Load(clip, true, bus)) {
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
            const AudioBus bus = ValidBus(a.Bus);
            const bool fresh = !src.V || src.Clip != a.Clip;
            bool resume = false;                 // a bus change: carry on where the old voice was
            ma_uint64 cursor = 0;
            if (!fresh && src.Bus != bus) {
                resume = ma_sound_is_playing(&src.V->Sound) == MA_TRUE;
                ma_sound_get_cursor_in_pcm_frames(&src.V->Sound, &cursor);
                src.V = I.Load(a.Clip, true, bus);
                src.Bus = bus;
                if (!src.V) continue;
                if (cursor > 0) ma_sound_seek_to_pcm_frame(&src.V->Sound, cursor);
            }
            else if (fresh) {
                src.V = I.Load(a.Clip, true, bus);   // spatialisation is switched per frame in Apply
                src.Clip = a.Clip;
                src.Bus = bus;
                if (!src.V) continue;
            }
            Impl::Apply(&src.V->Sound, a, glm::vec3(scene.GetWorldMatrix(e)[3]));   // world position (parents included)
            if (fresh && (a.PlayOnStart || I.PendingPlay.count(e))) ma_sound_start(&src.V->Sound);
            else if (I.PendingPlay.count(e)) { ma_sound_seek_to_pcm_frame(&src.V->Sound, 0); ma_sound_start(&src.V->Sound); }
            else if (resume) ma_sound_start(&src.V->Sound);
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

    // A stopped group node doesn't pull its sounds, so they hold their place until it starts again
    void AudioEngine::SetPaused(bool paused) {
        Impl& I = *m_Impl;
        if (!I.Ready || paused == I.Paused) return;
        I.Paused = paused;
        for (int i = 0; i < kAudioBusCount; i++) {
            if ((AudioBus)i == AudioBus::UI) continue;
            if (paused) ma_sound_group_stop(&I.Groups[i]);
            else        ma_sound_group_start(&I.Groups[i]);
        }
    }

    bool AudioEngine::IsPaused() const { return m_Impl->Paused; }

    void AudioEngine::SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) {
        if (!m_Impl->Ready) return;
        ma_engine_listener_set_position(&m_Impl->Engine, 0, position.x, position.y, position.z);
        ma_engine_listener_set_direction(&m_Impl->Engine, 0, forward.x, forward.y, forward.z);
        ma_engine_listener_set_world_up(&m_Impl->Engine, 0, up.x, up.y, up.z);
    }

    void AudioEngine::SetMasterVolume(float volume) { AudioMix m = m_Impl->Mix; m.Master = volume; m_Impl->SetMix(m); }
    float AudioEngine::GetMasterVolume() const { return m_Impl->Mix.Master; }
    void AudioEngine::SetMasterMuted(bool muted) { AudioMix m = m_Impl->Mix; m.MasterMuted = muted; m_Impl->SetMix(m); }
    bool AudioEngine::IsMasterMuted() const { return m_Impl->Mix.MasterMuted; }

    void AudioEngine::SetBusVolume(AudioBus bus, float volume) {
        AudioMix m = m_Impl->Mix;
        m.Volume[(int)ValidBus(bus)] = volume;
        m_Impl->SetMix(m);
    }
    float AudioEngine::GetBusVolume(AudioBus bus) const { return m_Impl->Mix.Volume[(int)ValidBus(bus)]; }
    void AudioEngine::SetBusMuted(AudioBus bus, bool muted) {
        AudioMix m = m_Impl->Mix;
        m.Muted[(int)ValidBus(bus)] = muted;
        m_Impl->SetMix(m);
    }
    bool AudioEngine::IsBusMuted(AudioBus bus) const { return m_Impl->Mix.Muted[(int)ValidBus(bus)]; }

    void AudioEngine::SetMix(const AudioMix& mix) { m_Impl->SetMix(mix); }
    const AudioMix& AudioEngine::GetMix() const { return m_Impl->Mix; }
    uint32_t AudioEngine::Revision() const { return m_Impl->Revision; }

    uint32_t AudioEngine::VoiceCount() const {
        return (uint32_t)(m_Impl->OneShots.size() + m_Impl->Sources.size());
    }

    uint32_t AudioEngine::VoiceCount(AudioBus bus) const {
        uint32_t n = 0;
        for (const auto& v : m_Impl->OneShots) if (v->Bus == bus) n++;
        for (const auto& [e, s] : m_Impl->Sources) if (s.V && s.Bus == bus) n++;
        return n;
    }

} // namespace Engine
