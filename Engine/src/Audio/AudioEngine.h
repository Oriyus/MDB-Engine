#pragma once
#include "Audio/AudioMix.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace Engine {

    class Scene;

    // Sound through miniaudio. Two kinds:
    //  - one-shots: Play / PlayAt, fire and forget (coin pickups, impacts). Cleaned up when done.
    //  - sources: one per entity with an AudioSourceComponent, kept in step with it by Update()
    //    (position, volume, pitch, bus). Started on appearance if PlayOnStart, or by PlaySource().
    // Clips are paths relative to the asset directory. Decoded audio is cached per file.
    // The listener is set by the app each frame (usually from the camera that is drawn).
    // Every sound plays on a bus (AudioMix.h): a miniaudio sound group with that bus's volume and mute,
    // under the master volume. Pause stops every bus but UI, so menu sounds still play.
    // If no audio device can be opened, everything here quietly does nothing (the mix is still kept).
    class AudioEngine {
    public:
        explicit AudioEngine(std::string assetDir);
        ~AudioEngine();
        AudioEngine(const AudioEngine&) = delete;
        AudioEngine& operator=(const AudioEngine&) = delete;

        bool IsReady() const;

        void Play(const std::string& clip, float volume = 1.0f, float pitch = 1.0f, AudioBus bus = AudioBus::SFX);
        void PlayAt(const std::string& clip, const glm::vec3& position, float volume = 1.0f, float pitch = 1.0f,
                    AudioBus bus = AudioBus::SFX);

        // Sync entity sources with the scene: create new ones, follow transforms and settings,
        // drop the ones whose entity or component is gone. Call every frame while playing.
        void Update(Scene& scene);
        void PlaySource(entt::entity e);        // (re)start from the beginning
        void StopSource(entt::entity e);
        bool IsSourcePlaying(entt::entity e) const;

        void StopAll();                         // every one-shot and source (call on Stop)
        void SetPaused(bool paused);            // freeze / resume everything but the UI bus (game pause)
        bool IsPaused() const;

        void SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);

        void SetMasterVolume(float volume);     // 0..1
        float GetMasterVolume() const;
        void SetMasterMuted(bool muted);
        bool IsMasterMuted() const;
        void SetBusVolume(AudioBus bus, float volume);   // 0..1
        float GetBusVolume(AudioBus bus) const;
        void SetBusMuted(AudioBus bus, bool muted);
        bool IsBusMuted(AudioBus bus) const;

        // The whole mix at once (the player's settings). Revision() goes up with every change that differs,
        // so an app can tell when to save it.
        void SetMix(const AudioMix& mix);
        const AudioMix& GetMix() const;
        uint32_t Revision() const;

        uint32_t VoiceCount() const;            // sounds currently alive (one-shots + sources)
        uint32_t VoiceCount(AudioBus bus) const;

    private:
        struct Impl;                            // keeps miniaudio.h out of everything else
        std::unique_ptr<Impl> m_Impl;
    };

} // namespace Engine
