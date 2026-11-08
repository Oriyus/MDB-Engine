#include "Runtime/GameSession.h"
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Scene/Prefab.h"
#include "Scene/SceneSerializer.h"
#include "Physics/PhysicsWorld.h"
#include "Audio/AudioEngine.h"
#include "Scripting/ScriptEngine.h"
#include "Renderer/Camera.h"
#include "Navigation/NavGrid.h"

namespace Engine {

    GameSession::GameSession(Scene& scene, const std::string& assetDir, const ResourceRegistry& resources)
        : m_Scene(scene) {
        m_Physics = std::make_unique<PhysicsWorld>();
        m_Nav = std::make_unique<NavGrid>();
        m_Audio = std::make_unique<AudioEngine>(assetDir);
        m_Prefabs = std::make_unique<PrefabLibrary>(assetDir, resources);
        m_Scripts = std::make_unique<ScriptEngine>(assetDir, m_Physics.get(), m_Audio.get());
        m_Scripts->SetResources(resources);
        m_Scripts->SetPrefabs(m_Prefabs.get());
        m_Scripts->SetNavigation(m_Nav.get());
    }

    GameSession::~GameSession() {
        if (m_Running) Stop();
    }

    void GameSession::Start() {
        if (m_Running) Stop();
        m_Physics->Reset();
        m_Nav->Clear();                  // the level may have changed: baked again on first use
        m_Scene.ResetParticles();        // emitters start over (the editor preview already ran them)
        m_Physics->Sync(m_Scene);
        m_Scripts->Start(m_Scene);
        m_Scene.BuildStaticBatches();    // after on_create, which may build or change the level
        m_Running = true;
    }

    void GameSession::Stop() {
        m_Scripts->Stop(m_Scene);        // first: on_destroy still sees the scene and its bodies
        m_Audio->StopAll();
        m_Scene.ClearStaticBatches();
        m_Nav->Clear();
        m_Physics->Reset();
        m_Running = false;
    }

    bool GameSession::IsRunning() const { return m_Running; }

    void GameSession::Update(float dt, bool singleStep) {
        if (!m_Running) return;
        const float stepDt = singleStep ? 1.0f / 60.0f : dt;
        m_Scene.OnUpdate(stepDt);                          // Spin / Orbit / animation
        m_Scripts->DispatchAnimationEvents(m_Scene);       // on_anim_event, "sound:" events
        m_Scripts->Update(m_Scene, stepDt);                // Lua on_update
        m_Physics->Step(m_Scene, stepDt, singleStep);      // physics overrides dynamic transforms
        m_Scripts->DispatchCollisions(m_Scene);            // on_collision_* / on_trigger_*
        m_Audio->Update(m_Scene);                          // entity sounds follow their transforms
    }

    void GameSession::SetPaused(bool paused) { m_Audio->SetPaused(paused); }

    bool GameSession::ConsumeRestartRequest() { return m_Scripts->ConsumeRestartRequest(); }

    bool GameSession::UpdateGameCamera(Camera& camera, float aspect) const {
        auto& reg = m_Scene.Registry();
        entt::entity chosen = entt::null;
        for (auto [e, c] : reg.view<CameraComponent>().each()) {
            if (chosen == entt::null) chosen = e;
            if (c.Primary) { chosen = e; break; }
        }
        if (chosen == entt::null) return false;

        glm::vec3 pos, scale;               // the camera's world placement (it may sit under a parent)
        glm::quat rot;
        Scene::Decompose(m_Scene.GetWorldMatrix(chosen), pos, rot, scale);
        const auto& c = reg.get<CameraComponent>(chosen);
        camera.SetAspect(aspect);
        camera.SetLens(c.FOV, c.Near, c.Far);
        camera.SetPose(pos, rot);
        return true;
    }

    void GameSession::SetListener(const Camera& camera) {
        m_Audio->SetListener(camera.Position, camera.GetFront(), glm::vec3(0.0f, 1.0f, 0.0f));
    }

    PhysicsWorld& GameSession::Physics() { return *m_Physics; }
    AudioEngine& GameSession::Audio() { return *m_Audio; }
    ScriptEngine& GameSession::Scripts() { return *m_Scripts; }
    PrefabLibrary& GameSession::Prefabs() { return *m_Prefabs; }
    NavGrid& GameSession::Navigation() { return *m_Nav; }

} // namespace Engine
