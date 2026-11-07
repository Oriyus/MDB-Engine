#pragma once
#include <memory>
#include <string>

namespace Engine {

    class Scene;
    class Camera;
    class PhysicsWorld;
    class AudioEngine;
    class ScriptEngine;
    class PrefabLibrary;
    struct ResourceRegistry;

    // Everything that makes a scene run as a game: physics, Lua scripts, audio and prefabs, stepped
    // in the right order. The editor runs one while in Play mode; the standalone game runs one all
    // the time. Snapshots (restoring the scene on Stop) are the caller's business.
    //
    // Per frame, Update does: Spin/Orbit -> scripts on_update -> physics step -> collision and
    // trigger callbacks -> entity sounds follow their transforms.
    class GameSession {
    public:
        GameSession(Scene& scene, const std::string& assetDir, const ResourceRegistry& resources);
        ~GameSession();
        GameSession(const GameSession&) = delete;
        GameSession& operator=(const GameSession&) = delete;

        void Start();                          // bodies first, then scripts (so on_create can use self.body)
        void Stop();                           // scripts (on_destroy), sounds and bodies; the scene stays as it is
        bool IsRunning() const;

        // Advance one frame. singleStep runs exactly one fixed physics step and shows its result
        // (for the editor's Step button).
        void Update(float dt, bool singleStep = false);
        void SetPaused(bool paused);           // silence the audio while the game is paused

        bool ConsumeRestartRequest();          // a script called scene.restart() this frame

        // Point `camera` at the scene's primary CameraComponent (or the first one), with the given
        // aspect ratio. False if the scene has no camera entity.
        bool UpdateGameCamera(Camera& camera, float aspect) const;
        void SetListener(const Camera& camera); // the ears follow the camera that is drawn

        PhysicsWorld& Physics();
        AudioEngine& Audio();
        ScriptEngine& Scripts();
        PrefabLibrary& Prefabs();

    private:
        Scene& m_Scene;
        std::unique_ptr<PhysicsWorld> m_Physics;
        std::unique_ptr<AudioEngine> m_Audio;          // before the script engine: it holds pointers to these
        std::unique_ptr<PrefabLibrary> m_Prefabs;
        std::unique_ptr<ScriptEngine> m_Scripts;
        bool m_Running = false;
    };

} // namespace Engine
