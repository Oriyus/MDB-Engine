#pragma once
#include "Scripting/ScriptEngine.h"      // ScriptHost
#include <memory>
#include <string>
#include <vector>

namespace Engine {

    class Scene;
    class Camera;
    class PhysicsWorld;
    class AudioEngine;
    class PrefabLibrary;
    class NavGrid;
    struct ResourceRegistry;
    struct SceneSettings;

    // Everything that makes a scene run as a game: physics, Lua scripts, audio and prefabs, stepped
    // in the right order. The editor runs one while in Play mode; the standalone game runs one all
    // the time. Snapshots (restoring the scene on Stop) are the caller's business.
    //
    // Per frame, Update does: Spin/Orbit -> scripts on_update -> physics step -> collision and
    // trigger callbacks -> entity sounds follow their transforms.
    //
    // It is also the campaign: the level being played, the levels visited before (each kept as its
    // differences from its scene file, see Runtime/LevelState.h), the scripts' `game` table, play time
    // and save slots. Loads, level changes and restarts are asked for during a frame (scripts, the
    // pause menu) and carried out by ApplyPending at the start of the next one.
    //
    // A save slot is one JSON file in the save directory:
    //   { "version": 1, "name", "time", "playTime", "level", "entry", "game": {...},
    //     "current": { "diff", "shared", "scripts" },            the level being played
    //     "visited": { "<scene file>": { "diff", "shared", "scripts" }... } }   levels left earlier
    class GameSession : public ScriptHost {
    public:
        GameSession(Scene& scene, const std::string& assetDir, const ResourceRegistry& resources);
        ~GameSession() override;
        GameSession(const GameSession&) = delete;
        GameSession& operator=(const GameSession&) = delete;

        void Start();                          // bodies first, then scripts (so on_create can use self.body)
        void Stop();                           // scripts (on_destroy), sounds and bodies; the scene stays as it is
        bool IsRunning() const;

        // Advance one frame. singleStep runs exactly one fixed physics step and shows its result
        // (for the editor's Step button).
        void Update(float dt, bool singleStep = false);
        void SetPaused(bool paused);           // silence the audio while the game is paused

        // ---- campaign
        // A fresh game on the live scene, which plays the level `levelFile` (a file in Assets/Scenes, or
        // "" for an unsaved scene). Call before Start when play begins: forgets visited levels and `game`.
        void NewGame(const std::string& levelFile);
        void SetSaveDirectory(const std::string& dir);   // created on the first save
        const std::string& SaveDirectory() const;
        // Carry out a load, level change or restart asked for since the last call. Call at the start of
        // a frame, before Update. True if the scene was replaced: `settings` (pass the current ones in;
        // what the scene file leaves out keeps them) then holds the new level's, for the renderer.
        // Gravity is applied already.
        bool ApplyPending(SceneSettings& settings);
        void RequestRestart();                 // the current level again, as it was entered (scene.restart too)
        bool SceneReplaced() const;            // ApplyPending loaded a scene since NewGame (the editor's settings changed)

        // ScriptHost (saves.* and level.* in Lua)
        bool SaveGame(const std::string& slot, const std::string& name, std::string& error) override;
        bool RequestLoad(const std::string& slot) override;
        bool DeleteSave(const std::string& slot) override;
        nlohmann::json ListSaves() const override;
        void RequestLevel(const std::string& scene, const std::string& entry, const std::vector<uint64_t>& carry) override;
        std::string CurrentLevel() const override;
        std::string LevelEntry() const override;
        bool Arrived() const override;
        float PlayTime() const override;

        // Point `camera` at the scene's primary CameraComponent (or the first one), with the given
        // aspect ratio. False if the scene has no camera entity.
        bool UpdateGameCamera(Camera& camera, float aspect) const;
        void SetListener(const Camera& camera); // the ears follow the camera that is drawn

        PhysicsWorld& Physics();
        AudioEngine& Audio();
        ScriptEngine& Scripts();
        PrefabLibrary& Prefabs();
        NavGrid& Navigation();             // baked on first use by a script (nav.*), cleared on Start and Stop

    private:
        struct Campaign;
        nlohmann::json CaptureLevel(const std::vector<uint64_t>& exclude, nlohmann::json* game);
        bool EnterLevel(const nlohmann::json& enter, SceneSettings& settings);

        Scene& m_Scene;
        std::unique_ptr<PhysicsWorld> m_Physics;
        std::unique_ptr<NavGrid> m_Nav;
        std::unique_ptr<AudioEngine> m_Audio;          // before the script engine: it holds pointers to these
        std::unique_ptr<PrefabLibrary> m_Prefabs;
        std::unique_ptr<ScriptEngine> m_Scripts;
        std::unique_ptr<Campaign> m_Campaign;
        bool m_Running = false;
    };

} // namespace Engine
