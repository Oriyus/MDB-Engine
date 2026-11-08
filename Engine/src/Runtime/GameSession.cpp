#include "Runtime/GameSession.h"
#include "Runtime/LevelState.h"
#include "Scene/Scene.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Scene/Prefab.h"
#include "Scene/SceneSerializer.h"
#include "Physics/PhysicsWorld.h"
#include "Audio/AudioEngine.h"
#include "Scripting/ScriptEngine.h"
#include "Renderer/Camera.h"
#include "Navigation/NavGrid.h"
#include "Navigation/FogOfWar.h"
#include "Core/Log.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace Engine {

    using json = nlohmann::json;

    namespace {
        // Slot names become file names: letters, digits, _ and -, at most 40
        bool ValidSlot(const std::string& slot) {
            if (slot.empty() || slot.size() > 40) return false;
            return std::all_of(slot.begin(), slot.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
            });
        }

        std::string NowString() {
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &t);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
            return buf;
        }

        bool ReadJson(const fs::path& path, json& out) {
            std::ifstream in(path);
            if (!in) return false;
            try { in >> out; }
            catch (const std::exception&) { return false; }
            return out.is_object();
        }
    }

    enum class PendingKind { None, Load, Level, Restart };

    struct GameSession::Campaign {
        std::string AssetDir;
        ResourceRegistry Resources;
        std::string SaveDir;

        std::string Level, Entry;            // the scene file being played ("" = an unsaved scene) and how it was entered
        float PlayTime = 0.0f;
        json Visited = json::object();       // scene file -> { diff, shared, scripts } as it was left
        json EntryState;                     // how the current level was entered (what a restart repeats)
        bool Replaced = false;
        bool Arrived = false;                // entered by level.change
        std::unordered_map<std::string, LevelBase> Bases;   // scene files as they load (diffs are against these)

        PendingKind Pending = PendingKind::None;
        std::string PendingSlot, PendingLevel, PendingEntry;
        std::vector<uint64_t> PendingCarry;

        fs::path SlotPath(const std::string& slot) const { return fs::path(SaveDir) / (slot + ".json"); }

        const LevelBase& Base(const std::string& level, PrefabLibrary* prefabs) {
            static const LevelBase empty;
            if (level.empty()) return empty;
            auto it = Bases.find(level);
            if (it != Bases.end()) return it->second;
            LevelBase base;
            if (!ReadLevelBase(AssetDir + "Scenes/" + level, Resources, prefabs, base))
                ENGINE_WARN("Save games: could not read Scenes/{}; its entities are saved in full", level);
            return Bases.emplace(level, std::move(base)).first->second;
        }
    };

    GameSession::GameSession(Scene& scene, const std::string& assetDir, const std::string& engineDir, const ResourceRegistry& resources)
        : m_Scene(scene) {
        m_Physics = std::make_unique<PhysicsWorld>();
        m_Nav = std::make_unique<NavGrid>();
        m_Fog = std::make_unique<FogOfWar>();
        m_Audio = std::make_unique<AudioEngine>(assetDir);
        m_Prefabs = std::make_unique<PrefabLibrary>(assetDir, resources);
        m_Scripts = std::make_unique<ScriptEngine>(assetDir, engineDir, m_Physics.get(), m_Audio.get());
        m_Scripts->SetResources(resources);
        m_Scripts->SetPrefabs(m_Prefabs.get());
        m_Scripts->SetNavigation(m_Nav.get());
        m_Scripts->SetFog(m_Fog.get());
        m_Scripts->SetHost(this);
        m_Campaign = std::make_unique<Campaign>();
        m_Campaign->AssetDir = assetDir;
        m_Campaign->Resources = resources;
    }

    GameSession::~GameSession() {
        if (m_Running) Stop();
    }

    void GameSession::Start() {
        if (m_Running) Stop();
        m_Physics->Reset();
        m_Nav->Clear();                  // the level may have changed: baked again on first use
        *m_Fog = FogOfWar{};             // a script turns fog on again (and loads what was explored)
        m_Scene.Fog = FogOfWarState{};
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
        *m_Fog = FogOfWar{};
        m_Scene.Fog = FogOfWarState{};
        m_Physics->Reset();
        m_Running = false;
    }

    bool GameSession::IsRunning() const { return m_Running; }

    void GameSession::Update(float dt, bool singleStep) {
        if (!m_Running) return;
        const float stepDt = singleStep ? 1.0f / 60.0f : dt;
        m_Campaign->PlayTime += stepDt;
        m_Scene.OnUpdate(stepDt);                          // Spin / Orbit / animation
        m_Scripts->DispatchAnimationEvents(m_Scene);       // on_anim_event, "sound:" events
        m_Scripts->Update(m_Scene, stepDt);                // Lua on_update
        m_Physics->Step(m_Scene, stepDt, singleStep);      // physics overrides dynamic transforms
        m_Scripts->DispatchCollisions(m_Scene);            // on_collision_* / on_trigger_*
        m_Audio->Update(m_Scene);                          // entity sounds follow their transforms
        if (m_Scripts->ConsumeRestartRequest()) RequestRestart();   // scene.restart(): next frame
    }

    void GameSession::SetPaused(bool paused) { m_Audio->SetPaused(paused); }

    // ------------------------------------------------------------------ campaign
    void GameSession::NewGame(const std::string& levelFile) {
        Campaign& c = *m_Campaign;
        c.Level = levelFile;
        c.Entry.clear();
        c.PlayTime = 0.0f;
        c.Visited = json::object();
        c.Bases.clear();                 // scene files may have been edited since the last game
        c.Replaced = false;
        c.Arrived = false;
        c.Pending = PendingKind::None;
        // A restart goes back to the scene as it is now: its file plus whatever differs (in the editor,
        // edits not saved yet and test entities)
        c.EntryState = { { "level", levelFile }, { "entry", "" },
                         { "diff", DiffLevel(m_Scene, c.Base(levelFile, m_Prefabs.get()), c.Resources) },
                         { "carried", json::array() }, { "restore", { { "game", json::object() } } }, { "loading", false } };
    }

    void GameSession::SetSaveDirectory(const std::string& dir) { m_Campaign->SaveDir = dir; }
    const std::string& GameSession::SaveDirectory() const { return m_Campaign->SaveDir; }
    bool GameSession::SceneReplaced() const { return m_Campaign->Replaced; }
    std::string GameSession::CurrentLevel() const { return m_Campaign->Level; }
    std::string GameSession::LevelEntry() const { return m_Campaign->Entry; }
    bool GameSession::Arrived() const { return m_Campaign->Arrived; }
    float GameSession::PlayTime() const { return m_Campaign->PlayTime; }

    void GameSession::RequestRestart() { m_Campaign->Pending = PendingKind::Restart; }

    void GameSession::RequestLevel(const std::string& scene, const std::string& entry, const std::vector<uint64_t>& carry) {
        Campaign& c = *m_Campaign;
        c.Pending = PendingKind::Level;
        c.PendingLevel = scene;
        c.PendingEntry = entry;
        c.PendingCarry = carry;
    }

    bool GameSession::RequestLoad(const std::string& slot) {
        Campaign& c = *m_Campaign;
        if (!ValidSlot(slot) || c.SaveDir.empty() || !fs::exists(c.SlotPath(slot))) return false;
        c.Pending = PendingKind::Load;
        c.PendingSlot = slot;
        return true;
    }

    // The level as it is now: { diff, shared, scripts }. Entities in `exclude` (and below them) and
    // their scripts are left out. game receives the `game` table.
    json GameSession::CaptureLevel(const std::vector<uint64_t>& exclude, json* game) {
        Campaign& c = *m_Campaign;
        std::unordered_set<uint64_t> roots(exclude.begin(), exclude.end()), all;
        for (uint64_t id : exclude) {
            const entt::entity h = m_Scene.FindByID(id);
            if (h != entt::null) for (entt::entity x : Subtree(m_Scene, h)) all.insert(m_Scene.GetID(x));
        }
        json scripts = m_Scripts->CaptureState([&](uint64_t uid) { return !all.contains(uid); });
        if (game) *game = scripts["game"];
        return { { "diff", DiffLevel(m_Scene, c.Base(c.Level, m_Prefabs.get()), c.Resources, roots) },
                 { "shared", scripts["shared"] }, { "scripts", scripts["scripts"] } };
    }

    bool GameSession::SaveGame(const std::string& slot, const std::string& name, std::string& error) {
        Campaign& c = *m_Campaign;
        if (!ValidSlot(slot)) { error = "a slot name is 1 to 40 letters, digits, _ or -"; return false; }
        if (c.SaveDir.empty()) { error = "no save directory"; return false; }
        if (!m_Running) { error = "the game is not running"; return false; }

        json game;
        json current = CaptureLevel({}, &game);
        json root = { { "version", 1 }, { "name", name }, { "time", NowString() }, { "playTime", c.PlayTime },
                      { "level", c.Level }, { "entry", c.Entry }, { "game", game },
                      { "current", std::move(current) }, { "visited", c.Visited } };

        std::error_code ec;
        fs::create_directories(c.SaveDir, ec);
        const fs::path path = c.SlotPath(slot), temp = path.string() + ".tmp";
        {
            std::ofstream out(temp);
            if (!out) { error = "could not write " + temp.string(); return false; }
            out << root.dump(1);
            if (!out) { error = "could not write " + temp.string(); return false; }
        }
        fs::rename(temp, path, ec);      // whole or not at all: a crash mid-write leaves the old save
        if (ec) { error = "could not replace " + path.string() + ": " + ec.message(); return false; }
        ENGINE_INFO("Saved the game to {} ({})", path.string(), name);
        return true;
    }

    bool GameSession::DeleteSave(const std::string& slot) {
        if (!ValidSlot(slot) || m_Campaign->SaveDir.empty()) return false;
        std::error_code ec;
        return fs::remove(m_Campaign->SlotPath(slot), ec);
    }

    json GameSession::ListSaves() const {
        const Campaign& c = *m_Campaign;
        std::vector<std::pair<fs::file_time_type, json>> found;
        std::error_code ec;
        if (c.SaveDir.empty() || !fs::is_directory(c.SaveDir, ec)) return json::array();
        for (const auto& f : fs::directory_iterator(c.SaveDir, ec)) {
            if (f.path().extension() != ".json") continue;
            json j;
            if (!ReadJson(f.path(), j)) continue;
            found.emplace_back(f.last_write_time(ec), json{ { "slot", f.path().stem().string() },
                { "name", j.value("name", std::string()) }, { "level", j.value("level", std::string()) },
                { "time", j.value("time", std::string()) }, { "play_time", j.value("playTime", 0.0) } });
        }
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        json out = json::array();
        for (auto& [t, j] : found) out.push_back(std::move(j));
        return out;
    }

    bool GameSession::ApplyPending(SceneSettings& settings) {
        Campaign& c = *m_Campaign;
        const PendingKind what = c.Pending;
        c.Pending = PendingKind::None;
        if (what == PendingKind::None || !m_Running) return false;

        json enter;
        if (what == PendingKind::Restart) {
            enter = c.EntryState;
        }
        else if (what == PendingKind::Level) {
            if (!fs::exists(c.AssetDir + "Scenes/" + c.PendingLevel)) {
                ENGINE_ERROR("level.change: there is no Scenes/{}", c.PendingLevel);
                return false;
            }
            // Carried entities leave with their children and their script data
            json carried = json::array();
            std::unordered_set<uint64_t> carriedIds;
            for (uint64_t id : c.PendingCarry) {
                const entt::entity root = m_Scene.FindByID(id);
                if (root == entt::null) continue;
                for (entt::entity x : Subtree(m_Scene, root)) {
                    json state = EntityState(m_Scene, x, c.Resources);
                    if (x == root) state.erase("parent");        // a root in the new level
                    carriedIds.insert(m_Scene.GetID(x));
                    carried.push_back(std::move(state));
                }
            }
            json game;
            c.Visited[c.Level] = CaptureLevel(c.PendingCarry, &game);
            json carriedScripts = m_Scripts->CaptureState([&](uint64_t uid) { return carriedIds.contains(uid); })["scripts"];

            const auto visited = c.Visited.find(c.PendingLevel);
            const bool seen = visited != c.Visited.end();
            json restore = { { "game", game }, { "scripts", seen ? (*visited)["scripts"] : json::object() } };
            if (seen) restore["shared"] = (*visited)["shared"];
            restore["scripts"].update(carriedScripts);
            enter = { { "level", c.PendingLevel }, { "entry", c.PendingEntry },
                      { "diff", seen ? (*visited)["diff"] : json(nullptr) }, { "carried", carried },
                      { "restore", restore }, { "loading", seen }, { "arrived", true } };
        }
        else {   // Load
            json save;
            if (!ReadJson(c.SlotPath(c.PendingSlot), save) || !save.contains("current")) {
                ENGINE_ERROR("Could not read save {}", c.SlotPath(c.PendingSlot).string());
                return false;
            }
            const json& cur = save["current"];
            c.Visited = save.value("visited", json::object());
            c.PlayTime = save.value("playTime", 0.0f);
            enter = { { "level", save.value("level", std::string()) }, { "entry", save.value("entry", std::string()) },
                      { "diff", cur.value("diff", json(nullptr)) }, { "carried", json::array() },
                      { "restore", { { "game", save.value("game", json::object()) },
                                     { "shared", cur.value("shared", json::object()) },
                                     { "scripts", cur.value("scripts", json::object()) } } },
                      { "loading", true } };
            ENGINE_INFO("Loading save {} ({})", c.PendingSlot, save.value("name", std::string()));
        }
        return EnterLevel(enter, settings);
    }

    // Replace the scene: the level's file, then its saved differences and the carried entities, then
    // Start with the scripts' state to restore. enter = { level, entry, diff, carried, restore, loading }.
    bool GameSession::EnterLevel(const json& enter, SceneSettings& settings) {
        Campaign& c = *m_Campaign;
        const std::string level = enter.value("level", std::string());
        Stop();
        bool ok = true;
        if (!level.empty())
            ok = SceneSerializer(m_Scene, c.Resources, m_Prefabs.get()).Load(c.AssetDir + "Scenes/" + level, &settings);
        else
            m_Scene.Clear();             // an unsaved scene: the diff holds every entity
        if (ok) {
            try {
                if (enter.contains("diff") && !enter["diff"].is_null()) ApplyLevelDiff(m_Scene, enter["diff"], c.Resources);
                for (const json& e : enter.value("carried", json::array())) ApplyEntityState(m_Scene, e, c.Resources);
            }
            catch (const std::exception& ex) {
                ENGINE_ERROR("Save games: a saved entity is malformed ({}); the level may be incomplete", ex.what());
            }
            c.Level = level;
            c.Entry = enter.value("entry", std::string());
            c.EntryState = enter;
            c.Arrived = enter.value("arrived", false);
            c.Replaced = true;
            m_Physics->SetGravity(settings.Gravity);
            m_Scripts->SetRestore(enter.value("restore", json::object()), enter.value("loading", false));
        }
        else {
            ENGINE_ERROR("Could not load Scenes/{}: the current level starts over", level);
        }
        Start();
        ENGINE_INFO("Entered level {}{}", level.empty() ? std::string("(unsaved scene)") : level,
            c.Entry.empty() ? std::string() : " at " + c.Entry);
        return ok;
    }

    // ------------------------------------------------------------------ cameras, listener, parts
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
