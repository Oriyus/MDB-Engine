#pragma once
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
    //    (position, volume, pitch). Started on appearance if PlayOnStart, or by PlaySource().
    // Clips are paths relative to the asset directory. Decoded audio is cached per file.
    // The listener is set by the app each frame (usually from the camera that is drawn).
    // If no audio device can be opened, everything here quietly does nothing.
    class AudioEngine {
    public:
        explicit AudioEngine(std::string assetDir);
        ~AudioEngine();
        AudioEngine(const AudioEngine&) = delete;
        AudioEngine& operator=(const AudioEngine&) = delete;

        bool IsReady() const;

        void Play(const std::string& clip, float volume = 1.0f, float pitch = 1.0f);
        void PlayAt(const std::string& clip, const glm::vec3& position, float volume = 1.0f, float pitch = 1.0f);

        // Sync entity sources with the scene: create new ones, follow transforms and settings,
        // drop the ones whose entity or component is gone. Call every frame while playing.
        void Update(Scene& scene);
        void PlaySource(entt::entity e);        // (re)start from the beginning
        void StopSource(entt::entity e);
        bool IsSourcePlaying(entt::entity e) const;

        void StopAll();                         // every one-shot and source (call on Stop)
        void SetPaused(bool paused);            // freeze / resume everything (game pause)

        void SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);
        void SetMasterVolume(float volume);     // 0..1
        float GetMasterVolume() const;

        uint32_t VoiceCount() const;            // sounds currently alive (one-shots + sources)

    private:
        struct Impl;                            // keeps miniaudio.h out of everything else
        std::unique_ptr<Impl> m_Impl;
    };

} // namespace Engine
